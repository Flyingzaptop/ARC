"""Refine an existing study without attaching, tracing, or enabling replacement."""
import argparse, csv, hashlib, importlib.util, json, math, struct
from pathlib import Path
from collections import defaultdict
import capstone
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM
from cpu_cfg_constants import analyze, canonical, NONVOLATILE
from cpu_sort_contract import preflight, compare_snapshot
from cpu_contract_events import event_records

def functions(path):
    blob=path.read_bytes()
    if len(blob)>16*1024*1024:raise ValueError('Code budget')
    count,=struct.unpack_from('<I',blob);offset=4
    if count>128:raise ValueError('Function budget')
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    result=[]
    for _ in range(count):
        rva,size=struct.unpack_from('<II',blob,offset);offset+=8
        code=blob[offset:offset+size];offset+=size
        if len(code)!=size:raise ValueError('Truncated code')
        instructions=list(decoder.disasm(code,rva))
        result.append((rva,code,instructions))
    if offset!=len(blob):raise ValueError('Trailing code')
    return result

def statistics(path):
    groups=defaultdict(list)
    with path.open(newline='') as f:
        for row in csv.DictReader(f):
            value=float(row['ms'])
            if not math.isfinite(value) or value<0:raise ValueError('Invalid timing')
            groups[row['event']].append((value,int(row['frame'])))
    result={}
    for name,items in groups.items():
        values=sorted(x[0] for x in items);peak=max(items)
        result[name]={'samples':len(values),'maximum_ms':peak[0],'maximum_frame':peak[1],
                      'p99_nearest_rank_ms':values[math.ceil(.99*len(values))-1],
                      'over_50ms':sum(x>50 for x in values),'over_100ms':sum(x>100 for x in values)}
    return result

def constant_audit(directory, hypothesis):
    sites={x['rva']:x for x in hypothesis['sites']};result=[]
    for rva,code,ins in functions(directory/'checked-functions.bin'):
        state,missing=analyze(ins)
        literals=defaultdict(set)
        for i in ins:
            if i.mnemonic in ('mov','movabs') and len(i.operands)==2 and i.operands[0].type==X86_OP_REG and i.operands[1].type==X86_OP_IMM:
                reg=canonical(i.reg_name(i.operands[0].reg))
                if reg in NONVOLATILE:literals[reg].add(i.operands[1].imm & ((1<<64)-1))
        assumed={r:next(iter(v)) for r,v in literals.items() if len(v)==1}
        for index,i in enumerate(ins):
            if i.address not in sites:continue
            known=state.get(i.address,{})
            following=[]
            for j in ins[index+1:index+4]:
                if j.group(capstone.CS_GRP_JUMP):following.append(j.mnemonic)
                else:break
            result.append({'rva':i.address,'function_rva':rva,
                'function_sha256':hashlib.sha256(code).hexdigest(),
                'unique_literal_assumptions':{r:hex(v) for r,v in assumed.items()},
                'must_constants_at_comparison':{r:hex(v) for r,v in known.items() if r in assumed},
                'unproved_assumptions':[r for r,v in assumed.items() if known.get(r)!=v],
                'unclosed_cfg_edges':missing,'flag_consumers':following,
                'comparison_interpretation':'unsigned' if following and all(x in ('jb','ja','jae','jbe','je','jne') for x in following) else 'unknown',
                'scope':'must-constants only; assumes Win64 callee preservation; does not certify key memory provenance or whole sort'})
    return result

def atomic_and_loops(directory,plan):
    meta=json.loads((directory/'capture.json').read_text());base=meta['main_base']
    stop=meta.get('semantic_stop_pc',0)
    observed=None
    # Only short stopped captures, never decode the 10.8M-event complete sort.
    if stop and meta['written_events']<=100000:
        decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
        for e in event_records(directory,meta):
            if e['rip']==stop:
                i=next(decoder.disasm(bytes.fromhex(e['code']),stop-base))
                observed={'rva':stop-base,'instruction':i.mnemonic+' '+i.op_str,
                          'classification':'retain original ordered CPU effect; not a rejection of surrounding computation'}
                break
    entry=meta['entry']-base;loops=[]
    for rva,code,ins in functions(plan/'checked-functions.bin'):
        if rva!=entry:continue
        for branch in ins:
            if not branch.group(capstone.CS_GRP_JUMP) or not branch.operands or branch.operands[0].type!=X86_OP_IMM:continue
            target=branch.operands[0].imm
            if not rva<=target<branch.address:continue
            body=[i for i in ins if target<=i.address<=branch.address]
            atomics=[i.address for i in body if i.mnemonic.startswith('lock ') or (i.mnemonic=='xchg' and any(o.type==X86_OP_MEM for o in i.operands))]
            calls=[i.address for i in body if i.group(capstone.CS_GRP_CALL)]
            loops.append({'start_rva':target,'backedge_rva':branch.address,'instructions':len(body),
                'direct_atomic_sites':atomics,'call_sites':calls,
                'investigate_inner_computation':not atomics,
                'admitted':False,'required_proof':['closed CFG and callee effects','memory ownership/aliasing',
                'publication before original synchronization and first consumer','live-in/live-out state'],
                'scope':'backedge interval inventory, not a single-entry extraction certificate'})
    return {'entry_rva':entry,'observed_atomic':observed,'inner_loop_inventory':loops,
            'entire_function_rejected_because_of_atomic':False}

def main():
    p=argparse.ArgumentParser();p.add_argument('study',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    selected=a.study/'final-training'/'candidate-00'
    hypothesis=json.loads((selected/'key-hypotheses.json').read_text())
    bits=hypothesis['hypotheses'][0]['projection']
    before=(selected/'input-span.bin').read_bytes();after=(selected/'output-span.bin').read_bytes()
    result={'schema':2,'base_commit':'4017d03','new_trace_collected':False,'replacement_enabled':False,
        'key_projection':bits,'key_comparison':'unsigned 64-bit; recorded local hypothesis, not complete algorithm proof',
        'snapshot_equivalence':compare_snapshot(before,after,bits),
        'restricted_input_preflight':preflight(before,bits),
        'comparison_constant_audit':constant_audit(selected,hypothesis),
        'ownership':{'status':'unknown','reason':'allocation escape and synchronization are not tracked across all threads',
          'insufficient_evidence':['private committed memory','TLS origin','disjoint entry spans','no observed conflict'],
          'required_method':'Track allocation identity and generation, pointer escape, publication and acquire/release across all relevant paths; alternatively enforce an exclusive lease covering input snapshot through result commit. A lease must also cover free/remap and asynchronous writers.'},
        'continuation':{'status':'observed_path_only','required_method':'Close caller live-out analysis over admitted continuations and preserve ABI/XSTATE/exception side effects.'},
        'candidates':{},'timing':{},'inputs':{}}
    for directory in sorted((a.study/'final-training').glob('candidate-*')):
        plan=a.study/'logical-plan'/directory.name
        result['candidates'][directory.name]=atomic_and_loops(directory,plan)
        result['timing'][directory.name]=statistics(directory/'cpu.csv')
        for file in ('capture.json','cpu.csv','checked-functions.bin','input-span.bin','output-span.bin'):
            path=directory/file
            if path.exists():result['inputs'][str(path.relative_to(a.study))]=hashlib.sha256(path.read_bytes()).hexdigest()
        path=plan/'checked-functions.bin';result['inputs'][str(path.relative_to(a.study))]=hashlib.sha256(path.read_bytes()).hexdigest()
    (a.output/'refinement.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:result[k] for k in ['snapshot_equivalence','restricted_input_preflight']},indent=2))
if __name__=='__main__':main()
