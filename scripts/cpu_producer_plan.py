"""Writer and bounded predecessor selection from hardware-watch evidence only."""
import json,sys
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_MEM,X86_OP_REG,X86_OP_IMM
GPRS=['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15']
def reg_index(name):return 16 if name=='rip' else -1 if not name else GPRS.index(name)
def memory(ins,op):
    if op.mem.segment:raise ValueError('segment-relative memory is outside capture class')
    m=op.mem;is_store=ins.mnemonic in ['mov','movss','movsd','movups','movaps','vmovss','vmovsd','vmovups','vmovaps','vmovdqu','vmovdqa','vpextrw','vpextrd','vpextrq','vextractps'] and ins.operands[0].type==X86_OP_MEM;return dict(base=reg_index(ins.reg_name(m.base)),index=reg_index(ins.reg_name(m.index)),scale=m.scale,disp=m.disp,size=op.size,write=is_store or bool(op.access&capstone.CS_AC_WRITE))
def address(m,registers,nextpc):
    get=lambda i:0 if i<0 else nextpc if i==16 else registers[i]
    return get(m['base'])+get(m['index'])*m['scale']+m['disp']
def plan(folder,out,calls=3,wait_ms=5000):
    if not 1<=calls<=4 or not 1<=wait_ms<=10000:raise ValueError("slice budget")
    observed=[json.loads(x) for x in (folder/'writes.jsonl').read_text().splitlines()]
    if not observed or any(not e['read_ok'] for e in observed):raise ValueError('insufficient write observations')
    if len({e['rip_after'] for e in observed})!=1:raise ValueError('ambiguous writer family')
    e=observed[0];d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True;code=(folder/'function-0.bin').read_bytes();ins=list(d.disasm(code,e['function_begin']))
    if len(ins)>2048:raise ValueError('static instruction budget')
    end=next(i for i,x in enumerate(ins) if x.address+x.size==e['rip_after']);writer=ins[end]
    outputs=[memory(writer,o) for o in writer.operands if o.type==X86_OP_MEM and memory(writer,o)['write']]
    outputs=[m for m in outputs if address(m,e['registers'],e['rip_after'])<=e['target']<address(m,e['registers'],e['rip_after'])+m['size']]
    if len(outputs)!=1:raise ValueError('ambiguous producer write')
    output=outputs[0];write_address=address(output,e['registers'],e['rip_after']);output_bytes=output['size']
    if end+1<len(ins):
        following=ins[end+1]
        contiguous=[memory(following,o) for o in following.operands if o.type==X86_OP_MEM and memory(following,o)['write']]
        if len(contiguous)==1 and address(contiguous[0],e['registers'],following.address+following.size)==write_address+output_bytes and output_bytes+contiguous[0]['size']<=e['bytes']:
            output_bytes+=contiguous[0]['size'];end+=1
    # Find the nearest conditional split with both paths converging before this store.
    begin=max(0,end-32)
    for i in range(end-1,max(-1,end-64),-1):
        x=ins[i]
        if x.group(capstone.CS_GRP_JUMP) and x.mnemonic!='jmp' and x.operands[0].type==X86_OP_IMM and x.address<x.operands[0].imm<=writer.address:
            begin=i-1 if i and ins[i-1].mnemonic in ['test','cmp'] else i;break
    if end-begin>64:raise ValueError('slice instruction budget')
    selected=ins[begin:end+1];instructions=[]
    for x in selected:
        mem=[memory(x,o) for o in x.operands if o.type==X86_OP_MEM and x.mnemonic!='lea']
        if len(mem)>2 or any(m['size']>32 for m in mem):raise ValueError('memory capture budget')
        instructions.append(dict(pc=x.address,size=x.size,code=x.bytes.hex(),mnemonic=x.mnemonic,operands=x.op_str,memory=mem))
    binding=json.loads((folder/'binding.json').read_text());p={'status':'copy_writer_with_bounded_predecessor_hypothesis','writer_pc':writer.address,'writer_rva':writer.address-e['module_base'],'writer':writer.mnemonic+' '+writer.op_str,'observed_tid':e['tid'],'entry':selected[0].address,'stop':ins[end].address+ins[end].size,'output_memory':output,'output_address':write_address,'output_bytes':output_bytes,'instructions':instructions,'binding':binding,'replacement_allowed':False}
    out.mkdir(parents=True,exist_ok=True);(out/'plan.json').write_text(json.dumps(p,indent=2))
    tids=sorted({json.loads(l)['tid'] for l in (folder/'writes.jsonl').read_text().splitlines()})
    head=[p['entry'],p['stop'],write_address,binding['relation']['cpu_stride'],output_bytes,calls,wait_ms,len(tids),*tids,len(instructions)]
    lines=[' '.join(str(v) for v in head),' '.join(str(output[k]) for k in ['base','index','scale','disp','size'])]
    for x in instructions:
        v=[x['pc'],x['size'],len(x['memory'])]
        for m in x['memory']:v.extend(m[k] for k in ['base','index','scale','disp','size'])
        lines.append(' '.join(map(str,v)))
    (out/'slice.txt').write_text('\n'.join(lines)+'\n');return p
if __name__=='__main__':
    p=plan(Path(sys.argv[1]),Path(sys.argv[2]));print(p['writer'],len(p['instructions']),'bounded instructions')
