"""Cheap name-blind GPU-work triage. Ranking is not transformation admission.

Unknown useful time, frequency and transfer volumes stay unknown. Samples rank
investigation within a feasibility tier; they never become milliseconds saved.
"""
import json,hashlib,math,importlib.util
from pathlib import Path
import capstone
from capstone.x86 import X86_OP_IMM
from cpu_cfg_constants import canonical

def assess(c):
    ops=[i['mnemonic'] for i in c['instructions']]
    escapes=[]
    for i in c['instructions']:
        if i['mnemonic']=='jmp':
            try:target=int(i['operands'],0)
            except ValueError:target=None
            if target is None or not c['loop_rva']<=target<=c['backedge_rva']:escapes.append(i['pc'])
    no_access={i['pc'] for i in c['instructions'] if i['mnemonic'] in ('nop','lea')}
    memory=[m for m in c['memory_operands'] if m['pc'] not in no_access];stores=[m for m in memory if m['write'] and m['base'] not in ('rsp','rbp')]
    calls=bool(c['call_sites']);atomic=any(x.startswith('lock ') for x in ops) or any(i['mnemonic']=='xchg' and any(m['pc']==i['pc'] for m in memory) for i in c['instructions'])
    exchanges=bool(c['record_exchanges']);carried=c['loop_carried_register_candidates']
    gpr_carried={canonical(r) for r in carried}-{None}-set(c['induction_candidates'])
    integer_reduce=False;integer_scan=False
    if len(gpr_carried)==1 and not calls and not c['interior_branches']:
        accumulator=next(iter(gpr_carried));updates=[]
        for i in c['instructions']:
            operands=i['operands'].split(',')
            if canonical(operands[0].strip())==accumulator and i['mnemonic'] not in ('cmp','test'):updates.append(i)
            if i['mnemonic'].startswith('mov') and len(operands)==2 and '[' in operands[0] and canonical(operands[1].strip())==accumulator:integer_scan=True
        integer_reduce=len(updates)==1 and updates[0]['mnemonic'] in ('add','and','or','xor')
        if integer_reduce:
            rhs=updates[0]['operands'].split(',')[-1].strip()
            if canonical(rhs)==accumulator or any(m['read'] and accumulator in (canonical(m.get('base') or ''),canonical(m.get('index') or '')) for m in memory):integer_reduce=False
    affine=bool(stores) and all(m['iteration_stride_candidate'] is not None and abs(m['iteration_stride_candidate'])>=m['size'] for m in stores)
    cross_conflicts=[]
    for index,a in enumerate(stores):
        for b in stores[index+1:]:
            stride=a['iteration_stride_candidate']
            if not stride or stride!=b['iteration_stride_candidate']:continue
            if any(a.get(k)!=b.get(k) for k in ('base','index','scale','segment')):continue
            center=(a['displacement']-b['displacement'])//stride
            for k in range(center-2,center+3):
                if k and max(a['displacement'],b['displacement']+k*stride)<min(a['displacement']+a['size'],b['displacement']+k*stride+b['size']):
                    cross_conflicts.append({'first_pc':a['pc'],'second_pc':b['pc'],'iteration_distance':k});break
    if cross_conflicts:affine=False
    vector_recurrence=any(x.startswith(('xmm','ymm','zmm')) for x in carried)
    scan='bsf' in ops and 'btc' in ops and bool(stores)
    # A reduction is only a hypothesis until operator/identity/order are certified.
    reduction=vector_recurrence and any(x in ('addps','vaddps','addss','vaddss','paddd','vpaddd','minps','vminps','maxps','vmaxps') for x in ops)
    if exchanges:
        kind='ordered_exchange';tier=0;algorithm=None
        parallel='No effective parallel transformation established for exact exchange order'
        serial=['data-dependent swaps and next-address decisions','equal-key permutation contract']
    elif atomic:
        kind='synchronization_or_atomic';tier=0;algorithm=None
        parallel='Inspect computation inside synchronization boundaries; do not move whole parent'
        serial=['original atomic ordering and publication']
    elif scan:
        kind='mask_expansion_scan';tier=3;algorithm='per-record popcount -> exclusive integer scan -> stable scatter in ascending bit order'
        parallel='Masks of different records can be counted and expanded independently after offsets'
        serial=['prefix-offset dependency replaced by parallel scan','batch boundaries, counters and consumer publication unclosed']
    elif integer_reduce:
        kind='integer_scan_hypothesis' if integer_scan else 'integer_reduction_hypothesis';tier=3
        algorithm='work-efficient ordered scan with original seed and modular width' if integer_scan else 'hierarchical integer reduction with original seed, width and overflow semantics'
        parallel='Independent inputs combined by a certified associative integer operator'
        serial=['scan/tree dependencies','final register and flags contract remains unclosed']
    elif affine and not calls and not c['interior_branches'] and not vector_recurrence and not gpr_carried:
        kind='affine_map';tier=4;algorithm='one GPU lane per independent element; procedural map if no data input'
        parallel='Element-local arithmetic with candidate fixed-stride output'
        serial=['cross-stream alias and induction/control proof still required']
    elif reduction and not calls:
        kind='reduction_hypothesis';tier=2;algorithm='hierarchical reduction only for certified associative operator and permitted numeric order'
        parallel='Partial per-tile reductions, then combine'
        serial=['combination tree','floating-point reassociation and NaN/signed-zero semantics not authorized']
    else:
        kind='unclosed_element_loop';tier=1;algorithm=None
        parallel='Element iteration visible; independent work not established'
        serial=['callee effects, address recurrence or loop-carried state unresolved']
    if calls and kind not in ('unclosed_element_loop','ordered_exchange'):serial.append('callee effects are unclosed; proposed topology covers only an inner slice')
    if escapes:
        tier=min(tier,1);serial.append('unconditional edge leaves backedge interval; recover real CFG before applying proposed primitive')
    times=[float(x['elapsed_ms']) for x in c.get('sampled_contexts',[])]
    read_bytes=sum(m['size'] for m in memory if m['read']);write_bytes=sum(m['size'] for m in memory if m['write'])
    return {'id':c['id'],'function_rva':c['function_rva'],'loop_rva':c['loop_rva'],'backedge_rva':c['backedge_rva'],
        'topology':kind,'feasibility_tier':tier,'sample_hits':c.get('samples',0),
        'parallel_work':parallel,'sequential_dependencies':serial,'gpu_algorithm':algorithm,
        'cross_output_iteration_conflicts':cross_conflicts,
        'non_induction_gpr_recurrence_candidates':sorted(gpr_carried),
        'unclosed_control_edges':escapes,
        'observed_activity':{'distinct_sample_times':len(set(times)),'span_ms':max(times)-min(times) if times else None,'scope':'activity only, not invocation frequency'},
        'numeric_requirements':'Floating reductions require exact NaN/signed-zero/order contract; integer tree/scan requires matching width, overflow and live-out flags' if reduction or scan or integer_reduce else 'preserve original arithmetic and live-out effects',
        'useful_cpu_ms':None,'calls_per_frame':None,'batch_elements':None,
        'static_access_bytes_per_iteration':{'read':read_bytes,'write':write_bytes,'scope':'sum of decoded operands, NOT traffic or transfer volume'},
        'exchange':{'input_bytes':None,'output_bytes':None,'upload_ms':None,'return_ms':None,'wait_ms':None,'gpu_consumer_proven':False,
                    'rules':['Count unique transferred live-in bytes, not decoded loads','If CPU consumes output, include full return and readiness wait','Fuse linked operations only after resource-edge/lifetime proof; do not double-count overlapping work']},
        'possible_fusion':'inspect adjacent producer/consumer edges; none established automatically',
        'benefit_ms':None,'detailed_capture_eligible':False,
        'next_action':'measure bounded invocation frequency, useful CPU time, input/output extents and first consumer' if tier>=2 else 'defer until effective parallel transform identified',
        'provenance':c.get('screen_origin','runtime_sampled_loop'),'manual_facts_used':False,
        'limits':['same-stream stride is not cross-output disjointness','sampling is not useful CPU cost','topology classification is a hypothesis, not a correctness certificate']}

def incorporate_measurements(ranked,measurements):
    """Accept measurements as ranking evidence, never as replacement permission."""
    for c in ranked:
        m=measurements.get(c['id'])
        c['economics_status']='unknown';c['net_task_ms_estimate']=None
        if not m:continue
        c['measurement_provenance']=m.get('provenance','unknown')
        if m.get('provenance')!='runtime_measured':
            c['manual_facts_used']=True;c['manual_note']=m
            continue # manual notes are visible but cannot silently drive automatic rank
        c['measurement_evidence']=m.get('evidence')
        fields=['cpu_useful_ms','calls_per_frame','batch_elements','input_bytes','output_bytes','upload_ms','return_ms','wait_ms','gpu_ms','dispatch_ms']
        if not m.get('evidence') or any(type(m.get(k)) not in (int,float) or not math.isfinite(m[k]) or m[k]<0 for k in fields):
            c['economics_status']='incomplete_measurements';continue
        c['useful_cpu_ms']=m['cpu_useful_ms'];c['calls_per_frame']=m['calls_per_frame'];c['batch_elements']=m['batch_elements']
        c['exchange'].update({k:m[k] for k in ['input_bytes','output_bytes','upload_ms','return_ms','wait_ms']})
        if m.get('cpu_time_scope')!='isolated_useful_running_time':
            c['economics_status']='parent_or_wait_time_not_useful_work';continue
        if m.get('gpu_consumer') and not m.get('consumer_edge_evidence'):
            c['economics_status']='gpu_residency_unsubstantiated';continue
        c['exchange']['gpu_consumer_proven']=bool(m.get('gpu_consumer') and m.get('consumer_edge_evidence'))
        c['exchange']['consumer_edge_evidence']=m.get('consumer_edge_evidence')
        total=sum(m[k] for k in ['upload_ms','return_ms','wait_ms','gpu_ms','dispatch_ms'])
        c['net_task_ms_estimate']=m['cpu_useful_ms']-total
        c['economics_status']='promising_task_not_frame_gain' if c['net_task_ms_estimate']>0 and m['calls_per_frame']>0 and m['batch_elements']>1 else 'unprofitable_or_not_recurring_large_batch'
        c['detailed_capture_eligible']=c['feasibility_tier']>=2 and c['economics_status']=='promising_task_not_frame_gain'
        if not c['detailed_capture_eligible']:c['next_action']='defer: insufficient economic or parallel evidence'

def ordering(c):
    rejected=c.get('economics_status') in ('unprofitable_or_not_recurring_large_batch','parent_or_wait_time_not_useful_work','gpu_residency_unsubstantiated')
    return (rejected,-int(c.get('detailed_capture_eligible',False)),-int(c['feasibility_tier']>=2),-int(c['sample_hits']>0),-c['feasibility_tier'],
            -int(c['exchange']['gpu_consumer_proven']),-(c.get('net_task_ms_estimate') or 0),-c['sample_hits'],c['backedge_rva']-c['loop_rva'],c['id'])

def build(probe,context=None,measurements=None):
    source=json.loads((probe/'candidates.json').read_text());candidates={c['id']:c for c in source['candidates']}
    if context:
        spec=importlib.util.spec_from_file_location('discovery',Path(__file__).with_name('discover-cpu-tasks.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        plan=json.loads((context/'selection.json').read_text())
        if plan['image_sha256']!=source['module_sha256']:raise ValueError('Context image generation mismatch')
        for entry in plan['candidates']:
            directory=context/entry['directory'];blob=(directory/'expected-code.bin').read_bytes()
            if len(blob)>65536:continue
            ins=list(m.md.disasm(blob,entry['entry_rva']));known={x.address for x in ins}
            for i in ins:
                if not i.group(capstone.CS_GRP_JUMP) or not i.operands or i.operands[0].type!=X86_OP_IMM:continue
                target=i.operands[0].imm
                if target not in known or not 0<i.address-target<=4096:continue
                identity=f'{source["module_sha256"][:16]}:{target:x}-{i.address:x}'
                if identity in candidates:continue
                loop=m.describe_loop(ins,target,i.address)
                candidates[identity]=dict(id=identity,function_rva=entry['entry_rva'],loop_rva=target,backedge_rva=i.address,samples=0,screen_origin='static_inner_loop_of_previously_sampled_function; execution/frequency unobserved',**loop)
    ranked=[assess(c) for c in candidates.values()]
    incorporate_measurements(ranked,measurements or {})
    ranked.sort(key=ordering)
    # Structural nested loops are not independent amounts of CPU time.
    for i,c in enumerate(ranked):
        c['rank']=i+1;c['overlap_ids']=[d['id'] for d in ranked if d['id']!=c['id'] and max(c['loop_rva'],d['loop_rva'])<=min(c['backedge_rva'],d['backedge_rva'])]
    return {'schema':1,'module_sha256':source['module_sha256'],'candidate_source_sha256':hashlib.sha256((probe/'candidates.json').read_bytes()).hexdigest(),
            'selection':'automatic feasibility-first cheap screen; unknown economics require bounded measurement before deep study',
            'manual_selection':False,'runtime_admission':False,'ranking_is_profit_prediction':False,
            'candidates':ranked,'recommended_cheap_measurement':next((x['id'] for x in ranked if x['feasibility_tier']>=2),None),
            'recommended_deep_capture':next((x['id'] for x in ranked if x['detailed_capture_eligible']),None)}

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('probe',type=Path);p.add_argument('output',type=Path);p.add_argument('--context',type=Path);p.add_argument('--measurements',type=Path);a=p.parse_args()
    measured=json.loads(a.measurements.read_text()) if a.measurements else {}
    if measured and measured['module_sha256']!=json.loads((a.probe/'candidates.json').read_text())['module_sha256']:raise ValueError('Measurement generation mismatch')
    result=build(a.probe,a.context,measured.get('candidates',{}));a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print([(x['rank'],x['id'],x['topology']) for x in result['candidates'][:5]])
