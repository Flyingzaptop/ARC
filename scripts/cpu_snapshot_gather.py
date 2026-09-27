"""Bounded snapshot gathering from composite address provenance.

Generated native code works only on cloned captured views and rechecks every
load against a region. Cursor/terminal guard leaves are packet preflight state,
not per-element CPU outputs.
"""
import argparse
import csv
import hashlib
import json
import mmap
import struct
from pathlib import Path

import capstone
from cpu_composite_ir import analyze_paths
from cpu_composite_gather import _dependencies
from cpu_producer_plan import GPRS


class Unsupported(ValueError):
    pass


def load_manifest(path):
    """Read a native snapshot manifest with portable region filenames."""
    path=Path(path)
    manifest=json.loads(path.read_text())
    for region in manifest['regions']:
        for phase in ('before','after'):
            key=phase+'_file';name=Path(region[key])
            if not name.is_absolute():region[key]=str((path.parent/name).resolve())
    return manifest


class Views:
    def __init__(self,manifest):
        self.regions=[]
        if not manifest.get('completed') or manifest.get('missed_regions'):
            raise Unsupported('incomplete snapshot views')
        for item in manifest['regions']:
            if not item['before_ok'] or item['size']<=0:raise Unsupported('unreadable before view')
            path=Path(item['before_file']);handle=path.open('rb')
            if path.stat().st_size!=item['size']:
                handle.close();raise Unsupported('view byte count mismatch')
            mapping=mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_READ)
            self.regions.append((item['base'],item['base']+item['size'],mapping,handle))
        self.regions.sort(key=lambda r:r[0])
        if any(a[1]>b[0] for a,b in zip(self.regions,self.regions[1:])):
            self.close();raise Unsupported('overlapping views')

    def read(self,address,size):
        if not 0<size<=32:raise Unsupported('load size')
        for begin,end,mapping,_ in self.regions:
            if begin<=address and address+size<=end:
                return mapping[address-begin:address-begin+size]
        raise Unsupported(f'address outside owned views: {address:#x}+{size}')

    def close(self):
        for _,_,mapping,handle in self.regions:
            mapping.close();handle.close()
        self.regions=[]

    def __enter__(self):return self
    def __exit__(self,*_):self.close()


def first_read_origins(typed):
    nodes=typed['nodes'];origins={}
    for access in typed['memory_accesses']:
        if access['kind']!='read':continue
        for node_id in _dependencies(nodes,access['value_source']):
            if nodes[node_id]['op']=='memory_input':
                origins.setdefault(node_id,access['address_source'])
    return origins


class Evaluator:
    def __init__(self,typed,manifest,views):
        self.nodes=typed['nodes'];self.manifest=manifest;self.views=views
        self.origins=first_read_origins(typed)
        self.cache={};self.row=0
        if manifest['induction_step']<=0 or manifest['packet_count']<=0:
            raise Unsupported('invalid induction')
        self.induction=manifest['induction_register']

    def start(self,row):
        if not 0<=row<self.manifest['packet_count']:raise Unsupported('row range')
        self.row=row;self.cache={}

    def value(self,node_id):
        if node_id in self.cache:return self.cache[node_id]
        if not 0<=node_id<len(self.nodes):raise Unsupported('node range')
        node=self.nodes[node_id];op=node['op'];width=len(bytes.fromhex(node['bytes']))
        def child(k):return self.value(node['inputs'][k] if isinstance(node['inputs'][k],int) else node['inputs'][k]['id'])
        def u(k):return int.from_bytes(child(k),'little')
        mask=(1<<(8*width))-1
        if op=='entry_register':
            raw=bytes.fromhex(node['bytes'])
            if node['name']==self.induction:
                raw=((int.from_bytes(raw,'little')+self.row*self.manifest['induction_step'])&mask).to_bytes(width,'little')
        elif op in ('entry_vector','immediate','constant','code_address'):
            raw=bytes.fromhex(node['bytes'])
        elif op=='captured_teb':raw=struct.pack('<Q',self.manifest['teb'])
        elif op=='memory_input':
            if node_id not in self.origins:raise Unsupported('memory leaf lacks original read')
            address=int.from_bytes(self.value(self.origins[node_id]),'little')
            raw=self.views.read(address,width)
        elif op in ('address','lea'):
            recipe=node['recipe'];parts=[u(i) for i in range(len(node['inputs']))]
            at=0;value=recipe['disp']
            if recipe.get('base'):
                value+=parts[at];at+=1
            if recipe.get('index'):
                value+=parts[at]*recipe['scale'];at+=1
            if recipe.get('segment')=='gs':value+=parts[at]
            elif recipe.get('segment'):raise Unsupported('segment')
            raw=(value&mask).to_bytes(width,'little')
        elif op in ('add','sub','shl'):
            a,b=u(0),u(1)
            value=(a+b) if op=='add' else (a-b) if op=='sub' else (a<<(b&63))
            raw=(value&mask).to_bytes(width,'little')
        elif op=='stack_adjust':
            before=int.from_bytes(bytes.fromhex(self.nodes[node['inputs'][0]]['bytes']),'little')
            after=int.from_bytes(bytes.fromhex(node['bytes']),'little')
            delta=(after-before)&mask
            raw=((u(0)+delta)&mask).to_bytes(width,'little')
        elif op=='extract_bytes':
            at=node['byte_offset'];raw=child(0)[at:at+node['width']]
        elif op=='pack_bytes':
            raw=bytes(self.value(ref['id'])[ref['byte_offset']] for ref in node['inputs'])
        else:raise Unsupported('address DAG operation '+op)
        if len(raw)!=width:raise Unsupported('computed width mismatch')
        self.cache[node_id]=raw
        return raw


def validate_first_paths(candidate,contract_path,manifest_path):
    manifest=load_manifest(manifest_path)
    contract=json.loads(Path(contract_path).read_text())
    models,_=analyze_paths(Path(candidate))
    if len(models)<2 or any(m['status']!='closed_observed_path' for m in models[:2]):
        raise Unsupported('first two paths not closed')
    leaves=[item['node'] for item in contract['inputs'] if item['origin']=='memory_input']
    with Views(manifest) as views:
        evaluator=Evaluator(models[0]['typed_ir'],manifest,views)
        checked=[];excluded=[]
        for row in range(2):
            evaluator.start(row)
            for node_id in leaves:
                expected=bytes.fromhex(models[row]['typed_ir']['nodes'][node_id]['bytes'])
                if node_id in (107,108,113):
                    excluded.append({'row':row,'node':node_id,'reason':'cursor/capacity/terminal preflight leaf'})
                    continue
                actual=evaluator.value(node_id)
                if actual!=expected:raise Unsupported(f'snapshot leaf mismatch row={row} node={node_id}: {actual.hex()} != {expected.hex()}')
                checked.append({'row':row,'node':node_id})
    return {'status':'validated_first_two_paths','checked_leaf_values':len(checked),'excluded_preflight_leaves':excluded,
            'packet_count':manifest['packet_count'],'snapshot_coherent_proven':False,'live_binding_allowed':False}


def packet_proofs(candidate,manifest_path):
    candidate=Path(candidate);manifest=load_manifest(manifest_path)
    meta=json.loads((candidate/'capture.json').read_text())
    plan=json.loads((candidate/'memory-plan.json').read_text())
    by_rva={item['rva']:item for item in plan['records']}
    models,groups=analyze_paths(candidate)
    if len(models)<2 or any(m['status']!='closed_observed_path' for m in models[:2]):
        raise Unsupported('tail paths not closed')
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    tail=[]
    for event in groups[0][-4:-1]:
        x=next(decoder.disasm(bytes.fromhex(event['code']),event['rip']),None)
        if x is None:raise Unsupported('tail decode')
        rva=x.address-meta['main_base'];record=by_rva.get(rva)
        if record is None or not event['code'].startswith(record['code']):
            raise Unsupported('tail code differs from checked plan')
        tail.append((event,x,rva,record['code']))
    if [x.mnemonic for _,x,_,_ in tail]!=['add','cmp','jne']:
        raise Unsupported('tail is not counted add/cmp/jne')
    add,compare,branch=[x for _,x,_,_ in tail]
    if len(add.operands)!=2 or add.operands[0].type!=capstone.x86_const.X86_OP_REG or add.operands[1].type!=capstone.x86_const.X86_OP_IMM:
        raise Unsupported('tail induction form')
    register=add.reg_name(add.operands[0].reg);step=add.operands[1].imm
    if register!=manifest['induction_register'] or step!=manifest['induction_step']:
        raise Unsupported('tail induction manifest mismatch')
    if compare.reg_name(compare.operands[0].reg)!=register or compare.operands[1].type!=capstone.x86_const.X86_OP_MEM:
        raise Unsupported('tail compare form')
    if len(branch.operands)!=1 or branch.operands[0].imm!=manifest['module_base']+manifest['loop_entry_rva']:
        raise Unsupported('tail backedge target')
    if branch.address+branch.size!=manifest['module_base']+manifest['loop_end_rva']:
        raise Unsupported('tail exit target')
    context_bytes=meta['context_bytes'];entry=(candidate/manifest['entry_context_file']).read_bytes();exit_=(candidate/manifest['exit_context_file']).read_bytes()
    if len(entry)!=context_bytes or len(exit_)!=context_bytes:raise Unsupported('context size')
    def reg(data,name):return struct.unpack_from('<Q',data,meta['rax_offset']+GPRS.index(name)*8)[0]
    def rip(data):return struct.unpack_from('<Q',data,meta['rip_offset'])[0]
    start=reg(entry,register);final=reg(exit_,register)
    if rip(entry)!=manifest['module_base']+manifest['loop_entry_rva'] or rip(exit_)!=manifest['module_base']+manifest['loop_end_rva']:
        raise Unsupported('context loop boundary')
    bound_access=next((item for item in tail[1][0]['memory'] if item['ok'] and item['size']==8),None)
    if bound_access is None:raise Unsupported('uncaptured loop bound')
    bound=int.from_bytes(bytes.fromhex(bound_access['bytes']),'little')
    if bound<start or (bound-start)%step or (bound-start)//step!=manifest['packet_count'] or final!=bound:
        raise Unsupported('counted loop bound mismatch')
    for row,events in enumerate(groups[:2]):
        if reg_context_event(events[0],register)!=start+row*step or events[-2]['rip']!=branch.address:
            raise Unsupported('sampled induction progression')
    with Views(manifest) as views:
        if views.read(bound_access['address'],8)!=struct.pack('<Q',bound):raise Unsupported('snapshot loop bound mismatch')
        initial_cursor=int.from_bytes(views.read(manifest['cursor_address'],8),'little')
        capacity=int.from_bytes(views.read(manifest['cursor_address']+8,8),'little')
        begin=int.from_bytes(views.read(manifest['cursor_address']-8,8),'little')
        snapshot_bounds=next([r['base'],r['base']+r['size']] for r in manifest['regions']
                             if r['base']<=initial_cursor<r['base']+r['size'])
        if initial_cursor!=manifest['initial_cursor'] or capacity!=manifest['capacity_end']:
            raise Unsupported('cursor/capacity preimage mismatch')
        required=manifest['packet_count']*manifest['record_stride']
        if manifest['record_stride']<=0 or initial_cursor>capacity or required>capacity-initial_cursor:
            raise Unsupported('no-growth packet preflight failed')
        if initial_cursor+required!=manifest['expected_final_cursor']:
            raise Unsupported('expected cursor delta mismatch')
    header_region=next((r for r in manifest['regions'] if r['base']<=manifest['cursor_address'] and
                        manifest['cursor_address']+8<=r['base']+r['size']),None)
    output_region=next((r for r in manifest['regions'] if r['base']<=initial_cursor and
                        initial_cursor+required<=r['base']+r['size']),None)
    if header_region is None or output_region is None:raise Unsupported('after-view output region')
    with Path(header_region['after_file']).open('rb') as stream:
        stream.seek(manifest['cursor_address']-header_region['base'])
        observed_final=int.from_bytes(stream.read(8),'little')
    if observed_final!=manifest['expected_final_cursor']:
        raise Unsupported('after-view final cursor mismatch')
    with Path(output_region['after_file']).open('rb') as stream:
        stream.seek(initial_cursor-output_region['base'])
        after_records=stream.read(required)
    expected_file=candidate/'expected-records.bin'
    if len(after_records)!=required or after_records!=expected_file.read_bytes():
        raise Unsupported('expected records differ from after view')
    return {'status':'counted_tail_and_no_growth_snapshot_verified','loop_bound_proof':
            {'status':'verified_counted_tail','register':register,'step':step,'count':manifest['packet_count'],
             'entry_value':start,'bound_value':bound,'exit_value':final,
             'entry_rip':rip(entry),'exit_rip':rip(exit_),
             'candidate':str(candidate.resolve()),
             'expected_code_sha256':hashlib.sha256((candidate/'expected-code.bin').read_bytes()).hexdigest(),
             'tail':[{'rva':rva,'code':code,'mnemonic':x.mnemonic,'operands':x.op_str} for _,x,rva,code in tail],
             'bound_read_address':bound_access['address']},
            'independent_after_view':{'final_cursor':observed_final,'record_bytes':required,
                                      'record_sha256':hashlib.sha256(after_records).hexdigest()},
            'packet_preflight':{'status':'verified_on_owned_before_view','initial_cursor':initial_cursor,
                                'capacity_end':capacity,'max_count':manifest['packet_count'],
                                'record_stride':manifest['record_stride'],'required_bytes':required,
                                'headroom_bytes':capacity-initial_cursor-required,
                                'begin':begin,
                                'cursor':initial_cursor,'capacity':capacity,
                                'snapshot_bounds':snapshot_bounds,
                                'header_range':[manifest['cursor_address']-8,manifest['cursor_address']+16]},
            'snapshot_coherent_proven':False,'live_replacement_allowed':False}


def reg_context_event(event,name):
    return event['registers'][GPRS.index(name)]


def emit_native(candidate,contract_path,manifest_path,output_dir,*,optimized=False,
                separate_diagnostics=False,packet_kind='append',word_proof_path=None):
    from cpu_snapshot_gather_native import emit_native as run
    return run(candidate,contract_path,manifest_path,output_dir,optimized=optimized,
               separate_diagnostics=separate_diagnostics,packet_kind=packet_kind,
               word_proof_path=word_proof_path)


def finalize_packet_proof(candidate,proof_path,ranges_csv,append_contract_path,output_path,
                          *,packed_input_path=None,shader_contract_path=None):
    """Attach observed bounded source ranges and rerun no-growth alias preflight."""
    from cpu_append_contract import preflight_packet_no_growth
    proof=json.loads(Path(proof_path).read_text())
    if proof.get('status')!='counted_tail_and_no_growth_snapshot_verified':
        raise Unsupported('packet proof not verified')
    manifest=json.loads((Path(candidate)/'native-manifest.json').read_text())
    ranges=[]
    with Path(ranges_csv).open(newline='') as stream:
        rows=list(csv.DictReader(stream))
    if not rows or len(rows)>len(manifest['regions']):raise Unsupported('source range count')
    for item in rows:
        lo=int(item['begin']);hi=int(item['end'])
        if lo>=hi or not any(r['base']<=lo<hi<=r['base']+r['size'] for r in manifest['regions']):
            raise Unsupported('source range outside one owned view')
        ranges.append((lo,hi))
    pre=proof['packet_preflight']
    args={'begin':pre['begin'],'cursor':pre['cursor'],'capacity':pre['capacity'],
          'snapshot_bounds':pre['snapshot_bounds'],'source_ranges':ranges,
          'header_range':pre['header_range']}
    contract=json.loads(Path(append_contract_path).read_text())
    result=preflight_packet_no_growth(contract,packet_count=pre['max_count'],**args)
    if not result['input_guards_pass'] or result['isolated_output_end']!=manifest['expected_final_cursor']:
        raise Unsupported('source alias or no-growth preflight failed: '+str(result['reason']))
    proof['packet_preflight_args']=args
    proof['append_preflight_result']=result
    proof['source_ranges_status']='bounded_native_gather_observed'
    if packed_input_path is not None:
        if shader_contract_path is None:raise Unsupported('shader contract required for packed input')
        packed=Path(packed_input_path);shader=json.loads(Path(shader_contract_path).read_text())
        words=shader['input_words_per_call'];expected_bytes=manifest['packet_count']*words*4
        if packed.stat().st_size!=expected_bytes:raise Unsupported('packed input byte count')
        expected_file=Path(candidate)/'expected-records.bin'
        if expected_file.stat().st_size!=manifest['packet_count']*manifest['record_stride']:
            raise Unsupported('independent expected-record byte count')
        proof['artifact_binding']={
            'packed_input':{'path':str(packed.resolve()),'bytes':expected_bytes,
                            'sha256':hashlib.sha256(packed.read_bytes()).hexdigest(),
                            'source':'bounded_native_before_view_gather'},
            'expected_records':{'path':str(expected_file.resolve()),'bytes':expected_file.stat().st_size,
                                'sha256':hashlib.sha256(expected_file.read_bytes()).hexdigest(),
                                'source':'independent_after_view_capture'}}
    Path(output_path).write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
    return proof


def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('contract',type=Path)
    p.add_argument('manifest',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--emit-native',action='store_true')
    p.add_argument('--optimized',action='store_true')
    p.add_argument('--separate-diagnostics',action='store_true')
    p.add_argument('--packet-kind',choices=('append','word_scatter'),default='append')
    p.add_argument('--word-proof',type=Path)
    p.add_argument('--finalize',action='store_true')
    p.add_argument('--proof',type=Path)
    p.add_argument('--ranges',type=Path)
    p.add_argument('--append-contract',type=Path)
    p.add_argument('--packed-input',type=Path)
    a=p.parse_args()
    if a.emit_native and a.finalize:raise Unsupported('choose one mode')
    if a.finalize:
        if not all((a.proof,a.ranges,a.append_contract,a.packed_input)):
            raise Unsupported('finalization requires proof, ranges, append contract, packed input')
        result=finalize_packet_proof(a.candidate,a.proof,a.ranges,a.append_contract,a.output,
                                     packed_input_path=a.packed_input,shader_contract_path=a.contract)
    elif a.emit_native:
        result=emit_native(a.candidate,a.contract,a.manifest,a.output,optimized=a.optimized,
                           separate_diagnostics=a.separate_diagnostics,packet_kind=a.packet_kind,
                           word_proof_path=a.word_proof)
    else:
        result=validate_first_paths(a.candidate,a.contract,a.manifest)
        a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))


if __name__=='__main__':main()
