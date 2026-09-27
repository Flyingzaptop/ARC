"""Copy captured VM views with only exact provenance inputs and declared outputs.

Original captures are read-only. Region files keep their sizes and offsets, but
every unmarked byte is zero. This is evidence packaging, not a live contract.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

from cpu_composite_ir import analyze_paths
from cpu_snapshot_gather import Evaluator, Views, first_read_origins


class RedactionError(ValueError):
    pass


def _sha256(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def rebase_native_manifest(redacted_candidate):
    """Repair only copied manifest paths after an archive is extracted elsewhere."""
    root=Path(redacted_candidate).resolve();path=root/'native-manifest.json'
    report=root/'redaction.json'
    if not report.exists() or json.loads(report.read_text()).get('status')!='exact_range_redacted_snapshot':
        raise RedactionError('refusing to rebase an unmarked original capture')
    if not path.exists():raise RedactionError('redacted native manifest missing')
    data=json.loads(path.read_text())
    for row in data['regions']:
        for phase in ('before','after'):
            target=root/Path(row[phase+'_file']).name
            if not target.exists() or target.stat().st_size!=row['size']:
                raise RedactionError('rebased region size/file missing')
            row[phase+'_file']=str(target)
    data['capture']=str(root)
    path.write_text(json.dumps(data,indent=2)+'\n')
    return path


class Marker:
    def __init__(self,regions):
        self.regions=regions
        self.masks=[bytearray(row['size']) for row in regions]

    def mark(self,address,size):
        if not 0<size<=1<<24:raise RedactionError('invalid marked span')
        matches=[i for i,row in enumerate(self.regions)
                 if row['base']<=address and address+size<=row['base']+row['size']]
        if len(matches)!=1:raise RedactionError(f'uncaptured source/output span {address:#x}+{size}')
        index=matches[0];offset=address-self.regions[index]['base']
        self.masks[index][offset:offset+size]=b'\x01'*size

    def counts(self):return [mask.count(1) for mask in self.masks]


class RedactionEvaluator(Evaluator):
    def __init__(self,typed,manifest,views,extra_induction=None):
        super().__init__(typed,manifest,views)
        self.extra_induction=extra_induction

    def value(self,node_id):
        node=self.nodes[node_id]
        if (self.extra_induction and node['op']=='entry_register' and
                node['name']==self.extra_induction[0]):
            if node_id not in self.cache:
                start,step=self.extra_induction[1:]
                width=len(bytes.fromhex(node['bytes']))
                self.cache[node_id]=((start+self.row*step)&((1<<(width*8))-1)).to_bytes(width,'little')
            return self.cache[node_id]
        return super().value(node_id)


def _copy_marked(source,destination,size,mask):
    source=Path(source);destination=Path(destination)
    if source.stat().st_size!=size:raise RedactionError('original view size changed')
    with source.open('rb') as src,destination.open('wb') as dst:
        dst.truncate(size)
        at=0
        while True:
            begin=mask.find(b'\x01',at)
            if begin<0:break
            end=mask.find(b'\x00',begin)
            if end<0:end=size
            src.seek(begin);data=src.read(end-begin)
            if len(data)!=end-begin:raise RedactionError('short source view')
            dst.seek(begin);dst.write(data)
            at=end
    if destination.stat().st_size!=size:raise RedactionError('redacted size mismatch')
    return _sha256(destination)


def bind_original_hashes(redacted_candidate):
    """Bind a validated redacted report to local originals without changing views."""
    redacted=Path(redacted_candidate).resolve();report_path=redacted/'redaction.json'
    if not report_path.exists():raise RedactionError('redaction report missing')
    report=json.loads(report_path.read_text())
    if report.get('status')!='exact_range_redacted_snapshot':raise RedactionError('redaction status')
    source=Path(report['source_candidate'])
    original=json.loads((source/'views.json').read_text())
    if len(original['regions'])!=len(report['regions']):raise RedactionError('region count changed')
    for entry,row in zip(report['regions'],original['regions']):
        if entry['base']!=row['base'] or entry['size']!=row['size']:
            raise RedactionError('region identity changed')
        for phase in ('before','after'):
            original_file=source/row[phase+'_file'];redacted_file=redacted/Path(row[phase+'_file']).name
            if original_file.stat().st_size!=row['size'] or redacted_file.stat().st_size!=row['size']:
                raise RedactionError('region file size changed')
            if _sha256(redacted_file)!=entry[phase+'_sha256']:
                raise RedactionError('redacted region bytes changed after native validation')
            entry[phase+'_original_sha256']=_sha256(original_file)
    report['original_hashes_bound']=True
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    return report


def redact(candidate,proof_path,output,*,kind):
    candidate=Path(candidate).resolve();output=Path(output).resolve()
    if output==candidate or candidate in output.parents or output in candidate.parents:
        raise RedactionError('redacted copy must be separate from original capture')
    if output.exists():raise RedactionError('output directory already exists')
    proof=json.loads(Path(proof_path).read_text())
    if Path(proof.get('candidate',candidate)).resolve()!=candidate:
        raise RedactionError('proof/candidate identity mismatch')
    views_json=json.loads((candidate/'views.json').read_text())
    if not views_json.get('completed') or views_json.get('missed_regions'):
        raise RedactionError('snapshot incomplete')
    regions=views_json['regions']
    if not regions or any(not row['before_ok'] or not row['after_ok'] for row in regions):
        raise RedactionError('view phase missing')
    models,_=analyze_paths(candidate)
    if len(models)<2 or any(model['status']!='closed_observed_path' for model in models[:2]):
        raise RedactionError('sample path not closed')
    first=models[0]['typed_ir'];other=models[1]['typed_ir']
    if [(n['op'],n['type'],n['inputs']) for n in first['nodes']]!=[(n['op'],n['type'],n['inputs']) for n in other['nodes']]:
        raise RedactionError('sample typed topology differs')
    origins=first_read_origins(first)
    all_leaves=[node for node in first['nodes'] if node['op']=='memory_input']
    if set(node['id'] for node in all_leaves)!=set(origins):
        raise RedactionError('memory leaf without original read')
    before=Marker(regions);after=Marker(regions)
    count=proof['loop_bound_proof']['count']
    if kind=='word_scatter':
        if proof.get('kind')!='word_scatter' or proof['status'] not in ('counted_word_scatter_snapshot_verified','word_scatter_snapshot_gather_bound'):
            raise RedactionError('word proof class/status')
        induction=proof['loop_bound_proof'];extra=(proof['output_preflight']['index_register'],
                                                   proof['output_preflight']['entry_index'],1)
        out=proof['output_preflight'];after.mark(out['output_start'],out['output_end']-out['output_start'])
        for item in proof['final_stack_metadata']:
            before.mark(item['address'],item['width']);after.mark(item['address'],item['width'])
        before.mark(out['base_pointer_read_address'],8)
    elif kind=='append':
        if proof.get('status') not in ('counted_tail_and_no_growth_snapshot_verified','word_scatter_snapshot_gather_bound'):
            raise RedactionError('append proof status')
        induction=proof['loop_bound_proof'];extra=None
        pre=proof['packet_preflight'];after.mark(pre['cursor'],pre['required_bytes'])
        h=pre['header_range'];before.mark(h[0],h[1]-h[0]);after.mark(h[0],h[1]-h[0])
    else:raise RedactionError('unknown redaction kind')
    manifest={**views_json,'regions':[],'packet_count':count,
              'induction_register':induction['register'],'induction_step':induction['step']}
    for row in regions:
        converted=dict(row)
        for phase in ('before','after'):
            converted[phase+'_file']=str((candidate/row[phase+'_file']).resolve())
        manifest['regions'].append(converted)
    with Views(manifest) as owned:
        evaluator=RedactionEvaluator(first,manifest,owned,extra)
        for row in range(count):
            evaluator.start(row)
            for node in all_leaves:
                address=int.from_bytes(evaluator.value(origins[node['id']]),'little')
                before.mark(address,len(bytes.fromhex(node['bytes'])))
    output.mkdir(parents=True)
    keep=('capture.json','events.bin','memory-plan.json','expected-code.bin','request.txt',
          'entry-context.bin','exit-context.bin','checked-functions.bin')
    for name in keep:
        path=candidate/name
        if path.exists():shutil.copy2(path,output/name)
    if kind=='append':
        expected=candidate/'expected-records.bin'
        if expected.exists():shutil.copy2(expected,output/expected.name)
        native=candidate/'native-manifest.json'
        if native.exists():
            restored=json.loads(native.read_text())
            restored['regions']=[{**row,
                'before_file':str((output/Path(row['before_file']).name).resolve()),
                'after_file':str((output/Path(row['after_file']).name).resolve())}
                for row in restored['regions']]
            restored['capture']=str(output)
            (output/'native-manifest.json').write_text(json.dumps(restored,indent=2)+'\n')
    redacted=[]
    for index,row in enumerate(regions):
        info={'index':index,'base':row['base'],'size':row['size']}
        for phase,marker in (('before',before),('after',after)):
            name=Path(row[phase+'_file']).name
            info[phase+'_retained_bytes']=marker.masks[index].count(1)
            info[phase+'_sha256']=_copy_marked(candidate/row[phase+'_file'],output/name,
                                                row['size'],marker.masks[index])
            info[phase+'_original_sha256']=_sha256(candidate/row[phase+'_file'])
        redacted.append(info)
    portable=dict(views_json)
    portable['regions']=[{**row,'before_file':Path(row['before_file']).name,
                              'after_file':Path(row['after_file']).name} for row in regions]
    portable['redacted_exact_provenance_class']=kind
    (output/'views.json').write_text(json.dumps(portable,indent=2)+'\n')
    report={'schema':1,'status':'exact_range_redacted_snapshot','kind':kind,
            'source_candidate':str(candidate),'packet_count':count,
            'memory_leaf_count':len(all_leaves),'regions':redacted,
            'total_before_retained_bytes':sum(x['before_retained_bytes'] for x in redacted),
            'total_after_retained_bytes':sum(x['after_retained_bytes'] for x in redacted),
            'same_size_region_files':True,'unmarked_bytes_zero':True,
            'original_hashes_bound':True,
            'scope':'supported captured path class only; original snapshots remain local'}
    (output/'redaction.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('proof',type=Path)
    p.add_argument('output',type=Path);p.add_argument('--kind',choices=('word_scatter','append'),required=True)
    args=p.parse_args();result=redact(args.candidate,args.proof,args.output,kind=args.kind)
    print(json.dumps({key:result[key] for key in ('status','kind','packet_count','total_before_retained_bytes','total_after_retained_bytes')}))


if __name__=='__main__':main()
