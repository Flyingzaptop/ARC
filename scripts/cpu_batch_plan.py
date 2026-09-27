"""Infer bounded caller loop and affine data recipes from observed machine code."""
import json,sys
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_REG,X86_OP_MEM,X86_OP_IMM
from cpu_producer_plan import GPRS,reg_index
def canonical(x):
    if x in GPRS:return x
    if x.startswith('e') and 'r'+x[1:] in GPRS:return 'r'+x[1:]
    if x.endswith('d') and x[:-1] in GPRS:return x[:-1]
    return x
def c(n):return ['constant',n]
def add(a,b):
    if b==c(0):return a
    if a==c(0):return b
    return ['add',a,b]
def mul(a,n):return a if n==1 else ['mul',a,c(n)]
def decode(blob,base):
    d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True;return list(d.disasm(blob,base))
def expression(ins,op,regs):
    if op.type==X86_OP_REG:return regs.get(canonical(ins.reg_name(op.reg)),['unknown'])
    if op.type==X86_OP_IMM:return c(op.imm)
    m=op.mem
    if m.segment:return ['unknown']
    base=regs.get(canonical(ins.reg_name(m.base)),['unknown']) if m.base else c(0)
    index=regs.get(canonical(ins.reg_name(m.index)),['unknown']) if m.index else c(0)
    address=add(add(base,mul(index,m.scale)),c(m.disp))
    return ['load',op.size,address]
def affine(expr):
    if expr[0]=='index':return c(0),1
    if expr[0]=='add':
        a,x=affine(expr[1]);b,y=affine(expr[2]);return add(a,b),x+y
    if expr[0]=='mul':
        a,x=affine(expr[1]);factor=expr[2][1];return mul(a,factor),x*factor
    return expr,0
def bytecode(e):
    if e[0]=='constant':return [(0,e[1])]
    if e==['arg0']:return [(1,0)]
    if e[0]=='add':return bytecode(e[1])+bytecode(e[2])+[(2,0)]
    if e[0]=='mul':return bytecode(e[1])+bytecode(e[2])+[(3,0)]
    if e[0]=='load' and e[1]==8:return bytecode(e[2])+[(4,0)]
    raise ValueError('unclosed pointer recipe '+str(e))
def forwarding(wrapper,child,index_offset):
    pointers={'rcx':('arg0',0),'rdx':('arg1',0),'rsp':('stack',0)};vectors={};memory={}
    def address(x,op):
        m=op.mem
        if m.index or x.reg_name(m.base) not in pointers:raise ValueError('wrapper address unresolved')
        base,offset=pointers[x.reg_name(m.base)];return base,offset+m.disp
    for x in wrapper:
        o=x.operands
        if x.mnemonic=='call':
            if o[0].type!=X86_OP_IMM or o[0].imm!=child:raise ValueError('wrapper target differs from observed producer')
            dest=pointers.get('rdx');prefix=0
            while dest and prefix<64 and memory.get((dest[0],dest[1]+prefix))==('arg1',prefix):prefix+=1
            if prefix<index_offset+4 or pointers.get('rcx',('',0))[0]!='arg0':raise ValueError('index forwarding not proven')
            return pointers['rcx'][1],prefix
        if x.mnemonic in ['sub','add'] and o[0].type==X86_OP_REG and o[1].type==X86_OP_IMM:
            name=x.reg_name(o[0].reg)
            if name not in pointers:raise ValueError('wrapper pointer mutation')
            base,n=pointers[name];pointers[name]=(base,n+o[1].imm*(1 if x.mnemonic=='add' else -1))
        elif x.mnemonic=='lea':pointers[x.reg_name(o[0].reg)]=address(x,o[1])
        elif x.mnemonic in ['vmovups','vmovsd','vmovss']:
            if o[1].type==X86_OP_MEM:
                base,n=address(x,o[1]);vectors[x.reg_name(o[0].reg)]=[(base,n+i) for i in range(o[1].size)]
            elif o[0].type==X86_OP_MEM:
                base,n=address(x,o[0]);v=vectors.get(x.reg_name(o[1].reg),[])
                if len(v)<o[0].size:raise ValueError('wrapper value missing')
                for i in range(o[0].size):memory[(base,n+i)]=v[i]
            else:raise ValueError('wrapper register conversion')
        else:raise ValueError('unsupported forwarding operation '+x.mnemonic)
    raise ValueError('no wrapper call')
def plan(watch,slice_dir,out,groups=3,max_elements=1024,wait_ms=5000):
    if not (1<=groups<=3 and 1<=max_elements<=1024 and 1<=wait_ms<=10000):raise ValueError('batch observation budget')
    hit=json.loads((watch/'writes.jsonl').read_text().splitlines()[0]);parent=json.loads((watch/'callers-0.json').read_text());fragment=json.loads((slice_dir/'plan.json').read_text());trace=[json.loads(x) for x in (slice_dir/'trace.jsonl').read_text().splitlines()];entry=trace[0]
    producer=decode((watch/'function-0.bin').read_bytes(),hit['function_begin']);regs={'rcx':['arg0'],'rdx':['arg1']};index_offset=None
    for x in producer:
        if x.address>=fragment['writer_pc']:break
        if not x.operands or x.operands[0].type!=X86_OP_REG:continue
        dest=canonical(x.reg_name(x.operands[0].reg));o=x.operands
        if x.mnemonic=='mov' and len(o)==2:
            e=expression(x,o[1],regs)
            if e[0]=='load' and e[1]==4 and e[2]==['arg1']:e=['index'];index_offset=0
            regs[dest]=e
        elif x.mnemonic=='lea':regs[dest]=expression(x,o[1],regs)[2]
        elif x.mnemonic=='shl' and o[1].type==X86_OP_IMM:regs[dest]=mul(regs.get(dest,['unknown']),1<<o[1].imm)
        elif x.mnemonic=='add':regs[dest]=add(regs.get(dest,['unknown']),expression(x,o[1],regs))
    output_reg=GPRS[fragment['output_memory']['base']];output_base,output_stride=affine(regs[output_reg]);output_base=add(output_base,c(fragment['output_memory']['disp']))
    executed={e['pc'] for e in trace}
    input_regs={GPRS[m['base']] for x in fragment['instructions'] if x['pc'] in executed for m in x['memory'] if m['base']>=0 and m['base']<16 and not m['write']}
    inputs=[]
    for name in input_regs:
        base,stride=affine(regs.get(name,['unknown']))
        if stride and name!=output_reg:inputs.append((name,base,stride))
    if len(inputs)!=1 or index_offset is None:raise ValueError('input affine model not unique')
    input_reg,input_base,input_stride=inputs[0]
    input_span=max(m['disp']+m['size'] for x in fragment['instructions'] for m in x['memory'] if m['base']==GPRS.index(input_reg) and not m['write'])
    if output_stride!=fragment['binding']['relation']['cpu_stride']:raise ValueError('code stride contradicts discovery')
    selected=None
    for depth,q in enumerate(parent):
        if not q['bytes']:continue
        ins=decode((watch/f'caller-0-{depth}.bin').read_bytes(),q['begin']);call=next((x for x in ins if x.address+x.size==q['return_pc'] and x.mnemonic=='call'),None)
        if not call:continue
        for j,x in enumerate(ins):
            if x.mnemonic!='jb' or x.operands[0].imm>call.address or x.address<call.address:continue
            cmp=ins[j-1];inc=ins[j-2]
            if cmp.mnemonic!='cmp' or inc.mnemonic!='inc':continue
            idx=canonical(inc.reg_name(inc.operands[0].reg));endreg=canonical(cmp.reg_name(cmp.operands[1].reg))
            if canonical(cmp.reg_name(cmp.operands[0].reg))!=idx:continue
            head=x.operands[0].imm
            initial=next((v for k,v in enumerate(ins[:-1]) if v.address<head and v.mnemonic=='cmp' and v.op_str==cmp.op_str and ins[k+1].mnemonic=='jae'),None)
            if not initial:continue
            loads={}
            for v in ins:
                if v.address>=initial.address:break
                if v.mnemonic=='mov' and v.operands[0].type==X86_OP_REG and v.operands[1].type==X86_OP_MEM:loads[canonical(v.reg_name(v.operands[0].reg))]=v.operands[1].mem
            if idx not in loads or endreg not in loads:continue
            a,b=loads[idx],loads[endreg]
            if a.base!=b.base or a.index or b.index:continue
            descriptor=canonical(initial.reg_name(a.base))
            for v in ins:
                if v.address>=initial.address:break
                if v.mnemonic=='mov' and all(o.type==X86_OP_REG for o in v.operands) and canonical(v.reg_name(v.operands[1].reg))==descriptor and canonical(v.reg_name(v.operands[0].reg)) in ['rbx','rbp','rsi','rdi','r12','r13','r14','r15']:
                    descriptor=canonical(v.reg_name(v.operands[0].reg));break
            defs={}
            for v in ins:
                if v.address>=call.address:break
                if v.mnemonic=='mov' and v.operands[0].type==X86_OP_REG and v.operands[1].type==X86_OP_MEM:defs[canonical(v.reg_name(v.operands[0].reg))]=v.operands[1].mem
            chain=[];mem=call.operands[0].mem
            while True:
                if mem.index:raise ValueError('indexed callback chain unsupported')
                chain.insert(0,mem.disp);base=canonical(call.reg_name(mem.base))
                if base==descriptor:break
                if base not in defs:raise ValueError('callback chain unresolved')
                mem=defs[base]
                if len(chain)>4:raise ValueError('callback chain budget')
            atomic=next((v for v in ins if v.address>x.address and v.mnemonic=='lock xadd'),None)
            if not atomic:raise ValueError('publication boundary not found')
            counter_load=next(v for v in ins if x.address<v.address<atomic.address and v.mnemonic=='mov' and v.operands[0].type==X86_OP_REG and canonical(v.reg_name(v.operands[0].reg))==canonical(atomic.reg_name(atomic.operands[0].mem.base)))
            selected=dict(depth=depth,entry=initial.address,call=call.address,returned=call.address+call.size,exit=atomic.address+atomic.size,index_reg=GPRS.index(idx),descriptor_reg=GPRS.index(descriptor),start_offset=a.disp,end_offset=b.disp,callback_chain=chain,callback_target=parent[depth-1]['begin'] if depth else hit['function_begin'],counter_offset=counter_load.operands[1].mem.disp,loop_code=[{'pc':v.address,'code':v.bytes.hex(),'text':v.mnemonic+' '+v.op_str} for v in ins]);break
        if selected:break
    if not selected:raise ValueError('no enclosing bounded callback loop')
    # The observed thin wrapper forwards RCX plus a constant and copies RDX bytes.
    wrapper=decode((watch/f"caller-0-{selected['depth']-1}.bin").read_bytes(),parent[selected['depth']-1]['begin'])
    arg_delta,prefix=forwarding(wrapper,hit['function_begin'],index_offset)
    row_index=(entry['output']-fragment['binding']['target'])//output_stride
    output0=fragment['output_address'];input0=entry['registers'][GPRS.index(input_reg)]-row_index*input_stride
    shared=[]
    for event in trace:
        node=next((x for x in fragment['instructions'] if x['pc']==event['pc']),None)
        if node and any(m['base']==16 for m in node['memory']):shared.extend(event['memory'])
    constant=shared[0] if shared else None
    result={'status':'local_callback_packet_hypothesis','loop':selected,'output_base_expression':output_base,'input_base_expression':input_base,'output_stride':output_stride,'input_stride':input_stride,'input_span':input_span,'output_bytes':fragment['output_bytes'],'expected_output_base':output0,'expected_input_base':input0,'arg0_adjust':arg_delta,'forwarded_argument_prefix_bytes':prefix,'index_argument_offset':index_offset,'shared_scalar':constant,'producer_pc':fragment['entry'],'producer_input_reg':GPRS.index(input_reg),'producer_output_reg':GPRS.index(output_reg),'producer_output_disp':fragment['output_memory']['disp'],'producer_condition_reg':GPRS.index(canonical(next(x for x in producer if x.address==fragment['entry']).reg_name(next(x for x in producer if x.address==fragment['entry']).operands[0].reg))),'producer_condition_value':entry['registers'][GPRS.index(canonical(next(x for x in producer if x.address==fragment['entry']).reg_name(next(x for x in producer if x.address==fragment['entry']).operands[0].reg)))],'global_task_extent':'unknown','replacement_allowed':False}
    ob,ib=bytecode(output_base),bytecode(input_base);out.mkdir(parents=True,exist_ok=True);(out/'plan.json').write_text(json.dumps(result,indent=2))
    loop=selected;tids=sorted({json.loads(x)['tid'] for x in (watch/'writes.jsonl').read_text().splitlines()});values=[loop['entry'],loop['call'],fragment['entry'],loop['exit']]+[loop[k] for k in ['index_reg','descriptor_reg','start_offset','end_offset','callback_target','counter_offset']]+[result[k] for k in ['producer_input_reg','producer_output_reg','producer_output_disp','producer_condition_reg','producer_condition_value']]+[arg_delta,index_offset,output0,input0,output_stride,input_stride,input_span,fragment['output_bytes'],groups,max_elements,wait_ms,len(tids),*tids,len(loop['callback_chain']),*loop['callback_chain'],len(ob)]
    text=' '.join(map(str,values))+'\n'+'\n'.join(f'{op} {v}' for op,v in ob)+'\n'+str(len(ib))+'\n'+'\n'.join(f'{op} {v}' for op,v in ib)+'\n';(out/'batch.txt').write_text(text);return result
if __name__=='__main__':
    r=plan(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]));print(r['loop'],r['input_stride'],r['output_stride'])
