"""Counted word-scatter snapshot proof and bounded gather manifest."""
import hashlib
import csv
import json
import struct
from pathlib import Path

from cpu_composite_gather import derive_homogeneous_group_contract, verify_group_bulk_views
from cpu_composite_ir import analyze_paths
from cpu_producer_plan import GPRS
from cpu_snapshot_gather import Evaluator, Views


class Unsupported(ValueError):
    pass


def packet_proofs(candidate,output_dir):
    candidate=Path(candidate);output_dir=Path(output_dir)
    views=json.loads((candidate/'views.json').read_text())
    meta=json.loads((candidate/'capture.json').read_text())
    contract=derive_homogeneous_group_contract(candidate)
    bulk=verify_group_bulk_views(candidate)
    paths,events=analyze_paths(candidate)
    if len(paths)<2 or any(p['status']!='closed_observed_path' or p['post_verified_final_external_bytes']!=4 for p in paths[:2]):
        raise Unsupported('first two word paths not byte-closed')
    code=json.loads((candidate/'memory-plan.json').read_text())
    checked={r['rva']:r['code'] for r in code['records']}
    tail=contract['capture_hints']['tail']
    if [item['mnemonic'] for item in tail]!=['add','cmp','jne'] or any(checked.get(item['rva'])!=item['code'] for item in tail):
        raise Unsupported('counted word tail differs from checked code')
    entry=(candidate/views['entry_context_file']).read_bytes();exit_=(candidate/views['exit_context_file']).read_bytes()
    if len(entry)!=meta['context_bytes'] or len(exit_)!=meta['context_bytes']:raise Unsupported('context size')
    def reg(data,name):return struct.unpack_from('<Q',data,meta['rax_offset']+GPRS.index(name)*8)[0]
    def rip(data):return struct.unpack_from('<Q',data,meta['rip_offset'])[0]
    hints=contract['capture_hints'];cursor_reg=hints['input_cursor_register'];bound_reg=hints['input_end_register']
    start=reg(entry,cursor_reg);bound=reg(entry,bound_reg);final=reg(exit_,cursor_reg)
    count=bulk['count'];step=hints['input_stride']
    if start+count*step!=bound or final!=bound or count!=hints['remaining_elements_observed']:
        raise Unsupported('word induction/count mismatch')
    if rip(entry)!=views['module_base']+views['loop_entry_rva'] or rip(exit_)!=views['module_base']+views['loop_end_rva']:
        raise Unsupported('word loop context mismatch')
    output_start=bulk['output_start'];output_end=output_start+bulk['output_bytes']
    output_region=next((r for r in views['regions'] if r['base']<=output_start and output_end<=r['base']+r['size']),None)
    if output_region is None:raise Unsupported('word output outside one owned view')
    after_path=candidate/output_region['after_file']
    with after_path.open('rb') as stream:
        stream.seek(output_start-output_region['base']);words=stream.read(bulk['output_bytes'])
    if len(words)!=bulk['output_bytes'] or hashlib.sha256(words).hexdigest()!=bulk['output_after_sha256']:
        raise Unsupported('after-view word hash mismatch')
    output_dir.mkdir(parents=True,exist_ok=False)
    expected=output_dir/'expected-words.bin';expected.write_bytes(words)
    normalized_regions=[]
    for item in views['regions']:
        row=dict(item)
        for key in ('before_file','after_file'):row[key]=str((candidate/item[key]).resolve())
        normalized_regions.append(row)
    local_manifest={**views,'regions':normalized_regions,'packet_count':count,
                    'induction_register':cursor_reg,'induction_step':step,'teb':views['teb']}
    def after_read(address,size):
        r=next((r for r in normalized_regions if r['base']<=address and address+size<=r['base']+r['size']),None)
        if r is None:raise Unsupported('stack after-view range')
        with Path(r['after_file']).open('rb') as stream:
            stream.seek(address-r['base']);raw=stream.read(size)
        if len(raw)!=size:raise Unsupported('stack after-view short read')
        return raw
    metadata=[];seen=set();first=paths[0]['typed_ir']
    with Views(local_manifest) as before_views:
        evaluator=Evaluator(first,local_manifest,before_views)
        evaluator.start(count-1)
        for write in contract['stack_write_recipes']:
            address,width=write['address'],write['width']
            if (address,width) in seen:continue
            seen.add((address,width))
            old=before_views.read(address,width);new=after_read(address,width)
            if address==bulk['counters'][0]['address'] and width==4:
                recipe={'kind':'initial_plus_emitted_count','initial_value':int.from_bytes(old,'little'),
                        'count_source':'GPU prefix emitted count'}
                if int.from_bytes(new,'little')!=int.from_bytes(old,'little')+count:
                    raise Unsupported('counter metadata mismatch')
            elif address==bulk['binary_flag']['address'] and width==1:
                recipe={'kind':'binary_initial_or_gpu_conditions','initial_value':old[0],
                        'select_nodes':[c['source'] for c in first['conditionals'] if c['predicate']=='cmova'],
                        'preflight':'initial flag in {0,1}'}
                if old[0] not in (0,1) or new[0] not in (0,1):raise Unsupported('flag metadata class')
            else:
                source=next((item['value_source'] for item in reversed(contract['stack_write_recipes'])
                             if item['address']==address and item['width']==width),None)
                if source is None or first['nodes'][source]['op']!='memory_input':
                    raise Unsupported('last-item metadata not direct captured leaf')
                expected_last=evaluator.value(source)
                if expected_last!=new:raise Unsupported('last-item metadata snapshot mismatch')
                recipe={'kind':'last_item_snapshot_leaf','typed_source_node':source,
                        'induction_row':count-1,'address_recipe':first['nodes'][source].get('address_recipe')}
            metadata.append({'address':address,'width':width,'before_hex':old.hex(),'after_hex':new.hex(),
                             'recipe':recipe})
    if len(metadata)!=5:raise Unsupported('final stack metadata range count')
    proof={'schema':1,'kind':'word_scatter','status':'counted_word_scatter_snapshot_verified',
           'candidate':str(candidate.resolve()),'expected_code_sha256':hashlib.sha256((candidate/'expected-code.bin').read_bytes()).hexdigest(),
           'loop_bound_proof':{'status':'verified_counted_tail','register':cursor_reg,'step':step,'count':count,
                               'entry_value':start,'bound_value':bound,'exit_value':final,
                               'entry_rip':rip(entry),'exit_rip':rip(exit_),'tail':tail},
           'output_preflight':{'status':'bounded_owned_snapshot_output','base_pointer_leaf':hints['output_base_pointer_leaf'],
                               'base_pointer_read_address':hints['output_base_pointer_read_address'],
                               'output_base':hints['output_base_observed'],
                               'index_register':contract['word_scatter']['index_register'],
                               'entry_index':bulk['entry_index'],'exit_index':bulk['exit_index'],
                               'word_stride':4,'output_start':output_start,'output_end':output_end,
                               'snapshot_bounds':[output_region['base'],output_region['base']+output_region['size']]},
           'counter_preflight':bulk['counters'],
           'binary_flag_preflight':bulk['binary_flag'],
           'binary_flag_select_proof':contract['binary_flag_select_proof'],
           'final_stack_metadata':metadata,
           'expected_words':{'path':str(expected.resolve()),'bytes':len(words),'sha256':hashlib.sha256(words).hexdigest(),
                             'source':'independent_after_view_capture'},
           'excluded_leaf_addresses':[bulk['counters'][0]['address'],bulk['binary_flag']['address']],
           'first_two_native_postimages_match':True,'homogeneous_guard_class_proven_for_full_count':False,
           'snapshot_coherent_proven':False,'live_replacement_allowed':False}
    (output_dir/'proof.json').write_text(json.dumps(proof,indent=2)+'\n')
    return proof


def gather_manifest(candidate,proof,output_path):
    candidate=Path(candidate);views=json.loads((candidate/'views.json').read_text())
    count=proof['loop_bound_proof']['count'];output=proof['output_preflight']
    regions=[]
    for item in views['regions']:
        row=dict(item)
        for key in ('before_file','after_file'):row[key]=str((candidate/item[key]).resolve())
        regions.append(row)
    manifest={**views,'regions':regions,'packet_count':count,
              'induction_register':proof['loop_bound_proof']['register'],
              'induction_step':proof['loop_bound_proof']['step'],
              'initial_cursor':output['output_start'],'expected_final_cursor':output['output_end'],
              'expected_words_file':proof['expected_words']['path']}
    Path(output_path).write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def finalize_gather(candidate,proof_path,contract_path,packed_path,ranges_path,output_path):
    candidate=Path(candidate);proof=json.loads(Path(proof_path).read_text())
    if proof.get('kind')!='word_scatter' or proof.get('status')!='counted_word_scatter_snapshot_verified' or Path(proof['candidate']).resolve()!=candidate.resolve():
        raise Unsupported('word proof identity')
    views=json.loads((candidate/'views.json').read_text())
    contract=json.loads(Path(contract_path).read_text())
    count=proof['loop_bound_proof']['count'];words=contract.get('input_words_per_row',contract.get('input_words_per_call'))
    if not isinstance(words,int) or not 0<words<=64:raise Unsupported('word input shape')
    packed=Path(packed_path);expected_bytes=count*words*4
    if packed.stat().st_size!=expected_bytes:raise Unsupported('packed word input byte count')
    first_two=packed.parent/'first-two-expected.bin'
    with packed.open('rb') as stream:prefix=stream.read(words*4*2)
    if first_two.read_bytes()!=prefix:raise Unsupported('first-two gathered leaves mismatch')
    with Path(ranges_path).open(newline='') as stream:rows=list(csv.DictReader(stream))
    if not rows or len(rows)>len(views['regions']):raise Unsupported('word source range count')
    sources=[];lo=proof['output_preflight']['output_start'];hi=proof['output_preflight']['output_end']
    for row in rows:
        begin,end=int(row['begin']),int(row['end'])
        if begin>=end or not any(region['base']<=begin<end<=region['base']+region['size'] for region in views['regions']):
            raise Unsupported('word source outside owned before view')
        if begin<hi and lo<end:raise Unsupported('word source aliases output reservation')
        sources.append([begin,end])
    proof['source_ranges']=sources
    proof['source_alias_preflight']={'status':'owned_nonaliasing_before_views','output_range':[lo,hi],
                                     'source_range_count':len(sources)}
    proof['artifact_binding']={'packed_input':{'path':str(packed.resolve()),'bytes':expected_bytes,
                                                'sha256':hashlib.sha256(packed.read_bytes()).hexdigest(),
                                                'source':'bounded_native_before_view_gather'},
                               'expected_words':proof['expected_words'],
                               'shader_contract_sha256':hashlib.sha256(Path(contract_path).read_bytes()).hexdigest()}
    proof['status']='word_scatter_snapshot_gather_bound'
    Path(output_path).write_text(json.dumps(proof,indent=2)+'\n')
    return proof
