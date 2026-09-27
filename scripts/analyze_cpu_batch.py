"""Validate inferred local packets; observed boundaries are not global ownership proof."""
import json, math, struct, sys
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_MEM
from cpu_producer_replay import run as replay

def scalar(b):
    v=struct.unpack('<f',b)[0]
    if not math.isfinite(v) or (v and abs(v)<2**-126):raise ValueError('unsupported arithmetic value')
    return v

def expression(node,read):
    if node.get('kind')=='memory_input':
        if node['bytes']!=4:raise ValueError('non-scalar source')
        return read(node['address'])
    if node.get('op') not in ('vaddss','vmulss'):raise ValueError('unclosed expression')
    operands=[]
    for key in ('left','right'):
        tags=node[key]
        if len(tags)!=4 or any(t!=tags[0] for t in tags):raise ValueError('mixed byte provenance')
        operands.append(scalar(expression(tags[0],read)))
    a,b=operands
    result=struct.pack('<f',a+b if node['op']=='vaddss' else a*b)
    scalar(result)
    return result

def consumers(folder):
    if not (folder/'writes.jsonl').exists():return {'status':'not_observed'}
    d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True;events=[]
    for e in map(json.loads,(folder/'writes.jsonl').read_text().splitlines()):
        path=folder/f"function-{e['event']}.bin"
        if not path.exists():continue
        code=list(d.disasm(path.read_bytes(),e['function_begin']))
        x=next((x for x in code if x.address+x.size==e['rip_after']),None)
        if x is None:continue
        mov=x.mnemonic.startswith(('mov','vmov'))
        role=('write' if x.operands[0].type==X86_OP_MEM else 'read') if mov else 'unknown'
        events.append({'event':e['event'],'qpc':e['qpc'],'tid':e['tid'],'pc':x.address,'function':e['function_begin'],'role':role,'instruction':x.mnemonic+' '+x.op_str})
    witnesses=[]
    for i,e in enumerate(events):
        if e['role']!='write':continue
        following=next((x for x in events[i+1:] if x['role'] in ('read','write')),None)
        if following and following['role']=='read':witnesses.append({'write':e,'first_observed_read':following,'same_thread_and_function':e['tid']==following['tid'] and e['function']==following['function']})
    return {'events':events,'witnesses':witnesses,'complete_thread_coverage':False,'absence_of_earlier_consumers_proven':False}

def analyze(root):
    bp=json.loads((root/'batch/plan.json').read_text());done=json.loads((root/'batch/done.json').read_text());groups=json.loads((root/'batch/groups.json').read_text())
    ir=replay(root/'slice')
    if not ir['exact'] or not ir['holdout_passed']:raise ValueError('fragment replay failed')
    call=ir['calls'][0];trace=[json.loads(x) for x in (root/'slice/trace.jsonl').read_text().splitlines()];entry=next(x for x in trace if x['call']==call['call'] and x['kind']==1)
    source=entry['registers'][bp['producer_input_reg']];constants={x['address']:bytes.fromhex(x['bytes']) for x in call['input_memory'] if not source<=x['address']<source+bp['input_span']}
    checks=[]
    for k,g in enumerate(groups):
        rows=g['rows'];valid=[r['index'] for r in rows]==list(range(g['start'],g['end']))
        mismatch=changed=later=0
        for r in rows:
            valid &= r['index']==r['argument_index'] and r['input_address']==bp['expected_input_base']+r['index']*bp['input_stride'] and r['output_address']==bp['expected_output_base']+r['index']*bp['output_stride']
            valid &= r['callback_begin_qpc']<=r['producer_qpc']<=r['callback_end_qpc']<=g['end_qpc']
            data=bytes.fromhex(r['input_ready'])
            def read(address):
                if source<=address<=source+len(data)-4:return data[address-source:address-source+4]
                if address in constants:return constants[address][:4]
                raise ValueError('unknown shared input')
            output=bytearray(bp['output_bytes']);covered=set()
            for part in call['output_provenance']:
                if part['bytes']!=4:raise ValueError('non scalar output')
                offset=part['offset'];output[offset:offset+4]=expression(part['expression'],read);covered.update(range(offset,offset+4))
            if len(covered)!=len(output):raise ValueError('partial output')
            mismatch+=output.hex()!=r['output_after'];changed+=r['input_before']!=r['input_ready'];later+=r['input_ready']!=r['input_after']
        checks.append({'role':'holdout' if k==len(groups)-1 else 'subsequent_validation','start':g['start'],'end':g['end'],'elements':len(rows),'bounds_and_addresses_valid':bool(valid),'output_mismatches':mismatch,'inputs_changed_inside_callback':changed,'inputs_changed_after_producer':later})
    clean=done['failure']==0 and done['armed']==done['restored'] and len(groups)>=2 and len(groups)==done['groups']
    return {'schema':1,'status':'validated_local_packets' if clean and all(x['bounds_and_addresses_valid'] and x['output_mismatches']==0 for x in checks) else 'invalid','groups':checks,'consumers':consumers(root/'consumers'),'global_task_extent':'unknown','frequency_per_frame':'unknown: bounded sample is selection biased','removable_cpu_ms':None,'gpu_ms':None,'probe_wall_time_is_cpu_work':False,'replacement_allowed':False,'contract':{'proved_from_code':['affine index recipes','bounded local loop','observed wrapper forwarding','atomic decrement exists after local loop'],'observed_only':['input freshness per callback','packet output replay','same function CPU consumption','subsequent other-thread reads'],'unknown':['all alternate paths','allocation lifetime and exclusive access','completion counter external wait and publication','complete global batch extent','cost of removable computation']},'missing_mechanism':'Recover upstream input producer and immediate CPU consumer as one schedulable region, or retain CPU execution; do not batch stale inputs or defer required reads.'}

if __name__=='__main__':
    result=analyze(Path(sys.argv[1]));Path(sys.argv[2]).write_text(json.dumps(result,indent=2)+'\n');print(result['status'],result['groups'])
