"""Package bounded CPU-composite evidence; bulk VM bytes come only from redacted copies."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from zipfile import ZIP_DEFLATED, ZipFile


TRACE_DIRS={
    'cpu-chain-direct-callee-20260927':'candidate-00',
    'cpu-chain-iterations-20260927':'candidate-01',
    'cpu-chain-iterations-holdout-20260927':'candidate-00',
    'cpu-composite-filter-train-20260927':'candidate-01',
    'cpu-composite-filter-positive-20260927':'candidate-01',
    'cpu-composite-filter-holdout-20260927':'candidate-01',
    'cpu-composite-pack-train-20260927':'candidate-00',
}
TRACE_FILES=('capture.json','events.bin','memory-plan.json','expected-code.bin',
             'checked-functions.bin','request.txt')
MEASUREMENTS=('append-original.json','word-original.json','append-gather.json',
              'word-gather.json','append-redacted-validation.json',
              'word-redacted-validation.json','targeted-tests.json')


class PackageError(ValueError):pass


def digest(path):
    result=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):result.update(block)
    return result.hexdigest()


def portable_proof(data,kind):
    data=json.loads(json.dumps(data))
    filter_capture='cpu-composite-bulk-covered-v2-20260927/candidate-01'
    word_capture='cpu-composite-pack-bulk-20260927/candidate-00'
    if kind=='filter':
        data['loop_bound_proof']['candidate']=filter_capture
        if 'artifact_binding' in data:
            data['artifact_binding']['packed_input']['path']='covered-v2-gather/input.bin'
            data['artifact_binding']['expected_records']['path']='covered-v2-expected-records.bin'
    elif kind=='word':
        data['candidate']=word_capture
        data['expected_words']['path']='word-scatter-bulk-proof-v2/expected-words.bin'
        if 'artifact_binding' in data:
            data['artifact_binding']['packed_input']['path']='word-gather-static/input.bin'
            data['artifact_binding']['expected_words']['path']='word-scatter-bulk-proof-v2/expected-words.bin'
    else:raise PackageError('unknown proof class')
    return data


def portable_manifest(data,kind):
    data=json.loads(json.dumps(data))
    for region in data['regions']:
        for phase in ('before','after'):
            region[phase+'_file']=Path(region[phase+'_file']).name
    data['capture']='.'
    data['module_image']='module-image.bin'
    data['entry_context_file']='entry-context.bin'
    data['exit_context_file']='exit-context.bin'
    if kind=='filter':data['append_contract']='append-contract.json'
    else:data['word_contract']='grouped-mask-contract-bulk2.json'
    return data


def package(source_root,redacted_filter,redacted_word,artifact_root,measure_root,
            module_image,output):
    source_root=Path(source_root);redacted_filter=Path(redacted_filter)
    redacted_word=Path(redacted_word);artifact_root=Path(artifact_root)
    measure_root=Path(measure_root);module_image=Path(module_image);output=Path(output)
    if output.exists():raise PackageError('output directory already exists')
    if not module_image.exists() or module_image.stat().st_size>32*1024*1024:
        raise PackageError('bounded module code image missing')
    for directory in (redacted_filter,redacted_word):
        report=json.loads((directory/'redaction.json').read_text())
        if report.get('status')!='exact_range_redacted_snapshot' or not report.get('original_hashes_bound'):
            raise PackageError('redacted VM report not hash-bound')
        for row in report['regions']:
            for phase in ('before','after'):
                path=directory/f"region-{row['index']}-{phase}.bin"
                if path.stat().st_size!=row['size'] or digest(path)!=row[phase+'_sha256']:
                    raise PackageError('redacted VM region changed')
    output.mkdir(parents=True)
    archive=output/'raw.zip';entries=[];names=set()
    with ZipFile(archive,'w',compression=ZIP_DEFLATED,compresslevel=6,allowZip64=True) as zipped:
        def add_bytes(name,data,category):
            posix=PurePosixPath(name)
            if posix.is_absolute() or '..' in posix.parts or name in names:
                raise PackageError('unsafe or duplicate archive member')
            names.add(name);zipped.writestr(name,data)
            entries.append({'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                            'category':category})
        def add_file(source,name,category,*,redacted_region=False):
            source=Path(source)
            if not source.is_file():raise PackageError(f'missing {source}')
            if source.name.startswith('region-') and not redacted_region:
                raise PackageError('unredacted VM region would enter archive')
            posix=PurePosixPath(name)
            if posix.is_absolute() or '..' in posix.parts or name in names:
                raise PackageError('unsafe or duplicate archive member')
            names.add(name);zipped.write(source,name)
            entries.append({'path':name,'bytes':source.stat().st_size,'sha256':digest(source),
                            'category':category})
        def add_json(source,name,category,transform):
            value=transform(json.loads(Path(source).read_text()))
            add_bytes(name,(json.dumps(value,indent=2)+'\n').encode(),category)
        for folder,candidate in TRACE_DIRS.items():
            root=source_root/folder/candidate;prefix=f'captures/{folder}/{candidate}'
            for filename in TRACE_FILES:
                path=root/filename
                if path.exists():add_file(path,f'{prefix}/{filename}','bounded_trace')
            if not (root/'capture.json').exists() or not (root/'events.bin').exists() or not (root/'memory-plan.json').exists():
                raise PackageError('required trace triplet missing: '+str(root))
        packet=source_root/'cpu-batch-final-20260927'
        for name in ('slice/plan.json','slice/trace.jsonl','batch/plan.json','batch/groups.json'):
            add_file(packet/name,'captures/cpu-batch-final-20260927/'+name,'bounded_packet_trace')
        for folder,candidate,root,kind in (
            ('cpu-composite-bulk-covered-v2-20260927','candidate-01',redacted_filter,'filter'),
            ('cpu-composite-pack-bulk-20260927','candidate-00',redacted_word,'word')):
            prefix=f'captures/{folder}/{candidate}'
            report=json.loads((root/'redaction.json').read_text())
            report['source_candidate']=f'{folder}/{candidate}'
            add_bytes(f'{prefix}/redaction.json',(json.dumps(report,indent=2)+'\n').encode(),'redaction_report')
            add_file(root/'views.json',f'{prefix}/views.json','redacted_view_manifest')
            for row in report['regions']:
                for phase in ('before','after'):
                    name=f"region-{row['index']}-{phase}.bin"
                    add_file(root/name,f'{prefix}/{name}','redacted_vm_region',redacted_region=True)
            for filename in TRACE_FILES+('entry-context.bin','exit-context.bin'):
                path=root/filename
                if path.exists():add_file(path,f'{prefix}/{filename}','bounded_bulk_trace')
            add_file(module_image,f'{prefix}/module-image.bin','bounded_code_image')
            if kind=='filter':
                add_file(root/'expected-records.bin',f'{prefix}/expected-records.bin','declared_output')
                add_file(source_root/folder/candidate/'append-contract.json',
                         f'{prefix}/append-contract.json','machine_contract')
                add_json(root/'native-manifest.json',f'{prefix}/native-manifest.json',
                         'portable_native_manifest',lambda d:portable_manifest(d,'filter'))
            else:
                add_file(artifact_root/'grouped-mask-contract-bulk2.json',
                         f'{prefix}/grouped-mask-contract-bulk2.json','machine_contract')
                add_json(root/'scatter-manifest.json',f'{prefix}/scatter-manifest.json',
                         'portable_native_manifest',lambda d:portable_manifest(d,'word'))
        filter_artifacts={
            'covered-v2-proof.json':'filter','covered-v2-final-proof.json':'filter',
            'word-scatter-bulk-proof-v2/proof.json':'word',
            'word-scatter-bulk-proof-v2/final-proof.json':'word'}
        for name,kind in filter_artifacts.items():
            add_json(artifact_root/name,f'artifacts/cpu-ir-gpu/{name}','portable_proof',
                     lambda d,k=kind:portable_proof(d,k))
        artifact_files=(
            'covered-v2-gather/input.bin','covered-v2-gather/source-ranges.csv',
            'covered-v2-expected-records.bin',
            'word-gather-static/input.bin','word-gather-static/source-ranges.csv',
            'word-scatter-bulk-proof-v2/expected-words.bin',
            'grouped-mask-contract-bulk2.json')
        for name in artifact_files:
            source=(redacted_filter/'expected-records.bin') if name=='covered-v2-expected-records.bin' else artifact_root/name
            add_file(source,f'artifacts/cpu-ir-gpu/{name}','bound_input_or_output')
        for filename in MEASUREMENTS:
            add_file(measure_root/filename,f'measurements/{filename}','native_measurement')
        for source,name in (
            ('experiments/cpu-composite-gpu/results/fused-tail.csv','append-fused-tail.csv'),
            ('experiments/cpu-word-gpu/results/word-fused-tail.csv','word-fused-tail.csv')):
            add_file(source,f'measurements/{name}','isolated_gpu_measurement')
    manifest={'schema':1,'archive':'raw.zip','raw_zip_bytes':archive.stat().st_size,
              'raw_zip_sha256':digest(archive),'entries':entries,
              'bulk_vm_policy':'Only exact-range redacted copies; original runtime heaps excluded.',
              'extract_layout':{'ARC_COMPOSITE_EVIDENCE_ROOT':'captures',
                                'ARC_COMPOSITE_ARTIFACT_ROOT':'artifacts/cpu-ir-gpu'}}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def main():
    p=argparse.ArgumentParser()
    for name in ('source-root','redacted-filter','redacted-word','artifact-root',
                 'measure-root','module-image','output'):
        p.add_argument('--'+name,required=True,type=Path)
    args=p.parse_args();result=package(args.source_root,args.redacted_filter,args.redacted_word,
                                      args.artifact_root,args.measure_root,args.module_image,args.output)
    print(json.dumps({'entries':len(result['entries']),'bytes':result['raw_zip_bytes'],
                      'sha256':result['raw_zip_sha256']}))


if __name__=='__main__':main()
