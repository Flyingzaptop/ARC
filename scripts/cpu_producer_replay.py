"""Exact float32 output replay for a small observed scalar-x86 class; never executes game code."""
import json,math,struct,sys
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_MEM,X86_OP_REG,X86_OP_IMM
from cpu_producer_plan import GPRS,reg_index

class Unsupported(Exception):pass
def replay_call(plan,events):
    if not events or events[0]['kind']!=1 or events[-1]['kind']!=2:raise Unsupported('incomplete observation')
    initial=events[0];regs=dict(zip(GPRS,initial['registers']));xmm=[bytes.fromhex(initial['xmm'])[i*16:i*16+16] for i in range(16)];flags=initial['flags'];shadow={};inputs=[];effects=[];memory_tags={}
    zero={'kind':'constant_zero'};register_tags={k:[{'kind':'live_register','name':k}]*8 for k in GPRS};vector_tags=[[{'kind':'live_register','name':f'xmm{i}'}]*16 for i in range(16)]
    if initial['mxcsr']&(0x6000|0x8040):raise Unsupported('rounding/FTZ/DAZ mode outside replay class')
    d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True;nodes={i['pc']:i for i in plan['instructions']};pc=initial['pc']
    def canonical(name):
        if name in regs:return name,64
        if name.startswith('e') and 'r'+name[1:] in regs:return 'r'+name[1:],32
        if name.endswith('d') and name[:-1] in regs:return name[:-1],32
        raise Unsupported('register '+name)
    for event in events[:-1]:
        if event['pc']!=pc or pc not in nodes:raise Unsupported('wrong producer/path')
        node=nodes[pc]
        if event.get('code')!=node['code']:raise Unsupported('code generation changed')
        x=next(d.disasm(bytes.fromhex(node['code']),pc));ops=x.operands;nextpc=pc+x.size
        def ea(op):
            m=op.mem
            def r(i):return 0 if not i else nextpc if x.reg_name(i)=='rip' else regs[canonical(x.reg_name(i))[0]]
            return r(m.base)+r(m.index)*m.scale+m.disp
        def value(op,n=None):
            n=n or op.size
            if op.type==X86_OP_REG:
                name=x.reg_name(op.reg)
                if name.startswith('xmm'):return xmm[int(name[3:])][:n]
                key,bits=canonical(name);return (regs[key]&((1<<bits)-1)).to_bytes(bits//8,'little')[:n]
            if op.type==X86_OP_IMM:return int(op.imm&((1<<(n*8))-1)).to_bytes(n,'little')
            if op.type==X86_OP_MEM:
                address=ea(op);read=next((m for m in event['memory'] if m['address']==address and m['size']>=n and m['ok']),None)
                if read is None:raise Unsupported('unclosed memory input')
                actual=bytes.fromhex(read['bytes'])[:n];result=bytes(shadow.get(address+i,actual[i]) for i in range(n))
                if result!=actual:raise ValueError('intermediate memory mismatch')
                if any(address+i not in shadow for i in range(n)):inputs.append({'pc':pc,'address':address,'bytes':actual.hex()})
                return result
            raise Unsupported('operand')
        def tags(op,n):
            if op.type==X86_OP_MEM:
                a=ea(op);return [memory_tags.get(a+i,{'kind':'memory_input','address':a,'bytes':n}) for i in range(n)]
            if op.type==X86_OP_IMM:return [{'kind':'immediate','value':op.imm}]*n
            name=x.reg_name(op.reg)
            return vector_tags[int(name[3:])][:n] if name.startswith('xmm') else register_tags[canonical(name)[0]][:n]
        def store(op,b,provenance=None):
            provenance=provenance or [{'kind':'derived_address_or_integer'}]*len(b)
            if op.type==X86_OP_MEM:
                a=ea(op)
                for i,v in enumerate(b):shadow[a+i]=v;memory_tags[a+i]=provenance[i]
                effects.append({'pc':pc,'address':a,'bytes':b.hex()});return
            name=x.reg_name(op.reg)
            if name.startswith('xmm'):xmm[int(name[3:])]=b.ljust(16,b'\0');vector_tags[int(name[3:])]=provenance+[zero]*(16-len(provenance));return
            key,bits=canonical(name);regs[key]=int.from_bytes(b[:bits//8],'little');register_tags[key]=provenance[:bits//8]+[zero]*(8-bits//8)
        m=x.mnemonic
        if m in ['test','cmp']:
            a=int.from_bytes(value(ops[0]),'little');b=int.from_bytes(value(ops[1],ops[0].size),'little');z=a&b if m=='test' else (a-b)&((1<<(8*ops[0].size))-1);flags=(flags&~0x40)|(0x40 if z==0 else 0)
        elif m in ['je','jne']:
            if bool(flags&0x40)==(m=='je'):nextpc=ops[0].imm
        elif m=='jmp':nextpc=ops[0].imm
        elif m=='lea':store(ops[0],ea(ops[1]).to_bytes(8,'little'))
        elif m=='mov':store(ops[0],value(ops[1],ops[0].size),tags(ops[1],ops[0].size))
        elif m in ['vmovss','vmovsd'] and len(ops)==2:
            n=4 if m=='vmovss' else 8;store(ops[0],value(ops[1],n),tags(ops[1],n))
        elif m in ['vaddss','vmulss'] and len(ops)==3:
            a=struct.unpack('<f',value(ops[1],4))[0];b=struct.unpack('<f',value(ops[2],4))[0]
            if not all(math.isfinite(v) and (v==0 or abs(v)>=2**-126) for v in [a,b]):raise Unsupported('non-normal arithmetic input')
            answer=a+b if m=='vaddss' else a*b
            try:low=struct.pack('<f',answer)
            except OverflowError:raise Unsupported('arithmetic overflow outside replay class')
            source=value(ops[1],16);node={'op':m,'precision':'float32','left':tags(ops[1],4),'right':tags(ops[2],4)};store(ops[0],low+source[4:16],[node]*4+tags(ops[1],16)[4:16])
        elif m=='add':store(ops[0],((int.from_bytes(value(ops[0]),'little')+int.from_bytes(value(ops[1],ops[0].size),'little'))&((1<<(ops[0].size*8))-1)).to_bytes(ops[0].size,'little'))
        else:raise Unsupported('unsupported operation '+m)
        pc=nextpc
    last=events[-1]
    if pc!=last['pc'] or pc!=plan['stop'] or not last['output_ok']:raise Unsupported('wrong exit/output')
    n=plan['output_bytes'];a=last['output']
    if any(a+i not in shadow for i in range(n)):raise Unsupported('output bytes not fully defined')
    expected=bytes(shadow[a+i] for i in range(n));actual=bytes.fromhex(last['value'])
    if expected!=actual:raise ValueError('new-input output mismatch')
    provenance=[]
    for i in range(n):
        tag=memory_tags[a+i]
        if provenance and provenance[-1]['expression']==tag:provenance[-1]['bytes']+=1
        else:provenance.append({'offset':i,'bytes':1,'expression':tag})
    serialized=json.dumps(provenance);arithmetic='"op": "vaddss"' in serialized or '"op": "vmulss"' in serialized;live_register='"kind": "live_register"' in serialized
    classification=('computation_with_live_register_inputs' if live_register else 'computation_from_captured_memory_inputs') if arithmetic else ('origin_unresolved_register_value' if live_register else 'copy_only')
    return {'call':initial['call'],'tid':initial['tid'],'input_qpc':initial['qpc'],'output_qpc':last['qpc'],'output_address':a,'output':actual.hex(),'input_memory':inputs,'memory_writes':effects,'output_provenance':provenance,'classification':classification,'status':'exact_output_replay','tolerance':0}
def run(folder):
    plan=json.loads((folder/'plan.json').read_text());rows=[json.loads(x) for x in (folder/'trace.jsonl').read_text().splitlines()];calls=[]
    for key in sorted({x['call'] for x in rows}):
        e=[x for x in rows if x['call']==key]
        try:r=replay_call(plan,e)
        except Unsupported as error:r={'call':key,'status':'unclosed_dependency','reason':str(error)}
        except ValueError as error:r={'call':key,'status':'replay_mismatch','reason':str(error)}
        r['role']='holdout' if key>=2 else 'reconstruction_check';calls.append(r)
    return {'calls':calls,'exact':bool(calls) and all(x['status']=='exact_output_replay' for x in calls),'holdout_passed':any(x['role']=='holdout' and x['status']=='exact_output_replay' for x in calls),'replacement_allowed':False,'effects_not_proven':['upper YMM/ZMM state','MXCSR exception flags for arbitrary inputs','upstream path and memory ownership','first external consumers']}
if __name__=='__main__':
    r=run(Path(sys.argv[1]));Path(sys.argv[2]).write_text(json.dumps(r,indent=2)+'\n');print([(x['call'],x['status'],x.get('reason')) for x in r['calls']])
