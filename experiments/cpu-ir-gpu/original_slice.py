"""Emit a guarded native reference for one observed straight-line x64 scalar slice.

The emitted bytes are only for owned-buffer, isolated measurement. This does not
validate live ownership, the surrounding callback, or removable frame work.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_MEM, X86_OP_REG


class UnsafeSlice(ValueError):
    pass


def build(root):
    plan=json.loads((root/'slice/plan.json').read_text())
    batch=json.loads((root/'batch/plan.json').read_text())
    trace=[json.loads(x) for x in (root/'slice/trace.jsonl').read_text().splitlines()]
    entry=next(x for x in trace if x['kind']==1)
    source=entry['registers'][batch['producer_input_reg']]
    inputs={m['address']:bytes.fromhex(m['bytes']) for e in trace if e['call']==entry['call'] for m in e.get('memory',[]) if m.get('ok')}
    nodes=plan['instructions']
    start=next((i for i,n in enumerate(nodes) if any(m['base']==16 for m in n['memory'])),None)
    if start is None:
        raise UnsafeSlice('no RIP scalar load')
    nodes=nodes[start:]
    if not nodes or nodes[-1]['pc']+nodes[-1]['size']!=plan['stop']:
        raise UnsafeSlice('non-contiguous slice end')
    if any(a['pc']+a['size']!=b['pc'] for a,b in zip(nodes,nodes[1:])):
        raise UnsafeSlice('instruction gap')
    if len(nodes)>32 or sum(n['size'] for n in nodes)>256:
        raise UnsafeSlice('slice budget')
    dis=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);dis.detail=True
    output_disp=plan['output_memory']['disp']
    if not 0<=output_disp<2**31 or plan['output_bytes']!=12:
        raise UnsafeSlice('output shape')
    rip_load=None;source_loads=[];scratch_writes=[];scratch_reads=[];output_writes=[]
    saw_scratch_address=False
    signatures=[]
    for index,n in enumerate(nodes):
        code=bytes.fromhex(n['code'])
        decoded=list(dis.disasm(code,n['pc']))
        if len(decoded)!=1 or decoded[0].size!=len(code) or decoded[0].mnemonic!=n['mnemonic']:
            raise UnsafeSlice('plan/decode mismatch')
        x=decoded[0];ops=x.operands
        signatures.append((x.mnemonic,tuple(x.reg_name(op.reg) if op.type==X86_OP_REG else
                                     ('addr:' if x.mnemonic=='lea' else 'mem:')+x.reg_name(op.mem.base)+':'+str(op.size)
                                     for op in ops)))
        if x.mnemonic not in {'vmovss','vaddss','vmulss','vmovsd','mov','lea'}:
            raise UnsafeSlice('unsupported instruction')
        if x.mnemonic=='lea':
            if x.op_str!='rax, [rbp]' or saw_scratch_address:raise UnsafeSlice('unsafe address derivation')
            saw_scratch_address=True
            continue  # LEA forms an address; it does not access memory.
        if x.mnemonic=='mov' and not any(op.type==X86_OP_MEM for op in ops):
            if x.op_str!='rcx, r15':raise UnsafeSlice('unsafe register operation')
        if x.mnemonic=='vmulss':
            if x.op_str not in ('xmm1, xmm1, xmm3','xmm1, xmm2, xmm3'):raise UnsafeSlice('unsafe arithmetic register')
        if x.mnemonic in ('vaddss','vmovss','vmovsd') and not any(op.type==X86_OP_MEM for op in ops):
            raise UnsafeSlice('unexpected register-only move/add')
        for op_index,op in enumerate(ops):
            if op.type!=X86_OP_MEM:continue
            if x.addr_size!=8 or op.mem.segment or len(n['memory'])!=1 or op.mem.index or op.mem.scale not in (0,1):
                raise UnsafeSlice('indexed or multiple memory operand')
            base=x.reg_name(op.mem.base);offset=op.mem.disp
            write=op_index==0 and x.mnemonic in ('vmovss','vmovsd','mov')
            if base=='rip':
                if index!=0 or write or op.size!=4 or x.mnemonic!='vmovss' or x.reg_name(ops[0].reg)!='xmm3' or x.disp_size!=4:
                    raise UnsafeSlice('RIP load outside supported scalar literal')
                address=x.address+x.size+offset
                if address not in inputs or len(inputs[address])!=4:raise UnsafeSlice('uncaptured RIP literal')
                rip_load=(index,x.disp_offset,inputs[address])
            elif base=='r15':
                if write or op.size!=4 or not 0<=offset<=batch['input_span']-4 or x.mnemonic not in ('vmovss','vaddss'):
                    raise UnsafeSlice('source access outside captured span')
                source_loads.append(offset)
            elif base=='rbp':
                if not write or op.size!=4 or offset not in (0,4,8) or x.mnemonic!='vmovss':
                    raise UnsafeSlice('scratch access')
                scratch_writes.append(offset)
            elif base=='rax':
                if write or not saw_scratch_address or (offset,op.size) not in ((0,8),(8,4)):
                    raise UnsafeSlice('scratch read')
                scratch_reads.append((offset,op.size))
            elif base=='rdi':
                if not write or (offset,op.size) not in ((output_disp,8),(output_disp+8,4)):
                    raise UnsafeSlice('output access')
                output_writes.append((offset,op.size))
            else:raise UnsafeSlice('foreign memory base '+base)
        if x.mnemonic=='mov' and x.op_str.startswith('eax,') and not any(op.type==X86_OP_MEM for op in ops):
            raise UnsafeSlice('unsafe integer move')
    if rip_load is None or sorted(source_loads)!=[0,4,8,16,20,24] or sorted(scratch_writes)!=[0,4,8] or sorted(scratch_reads)!=[(0,8),(8,4)] or sorted(output_writes)!=[(output_disp,8),(output_disp+8,4)]:
        raise UnsafeSlice('unsupported memory effect set')
    # This narrow instruction sequence is checked in order to bound register
    # effects as well as address effects. No branch/call/ret is accepted.
    sequence=[n['mnemonic'] for n in nodes]
    if sequence!=['vmovss','vmovss','vaddss','vmovss','vaddss','vmovss','vmulss','vmovss','vmulss','vaddss','vmovss','vmulss','vmovss','lea','mov','vmovsd','mov','vmovsd','mov']:
        raise UnsafeSlice('unsupported instruction order')
    expected_signatures=[
        ('vmovss',('xmm3','mem:rip:4')),
        ('vmovss',('xmm0','mem:r15:4')),
        ('vaddss',('xmm1','xmm0','mem:r15:4')),
        ('vmovss',('xmm0','mem:r15:4')),
        ('vaddss',('xmm2','xmm0','mem:r15:4')),
        ('vmovss',('xmm0','mem:r15:4')),
        ('vmulss',('xmm1','xmm1','xmm3')),
        ('vmovss',('mem:rbp:4','xmm1')),
        ('vmulss',('xmm1','xmm2','xmm3')),
        ('vaddss',('xmm2','xmm0','mem:r15:4')),
        ('vmovss',('mem:rbp:4','xmm1')),
        ('vmulss',('xmm1','xmm2','xmm3')),
        ('vmovss',('mem:rbp:4','xmm1')),
        ('lea',('rax','addr:rbp:8')),
        ('mov',('rcx','r15')),
        ('vmovsd',('xmm0','mem:rax:8')),
        ('mov',('eax','mem:rax:4')),
        ('vmovsd',('mem:rdi:8','xmm0')),
        ('mov',('mem:rdi:4','eax')),
    ]
    if signatures!=expected_signatures:
        raise UnsafeSlice('unsupported register effect signature')
    if not 12<=batch['input_span']<=4096 or batch['input_stride']<batch['input_span']:
        raise UnsafeSlice('source bounds')
    original=bytearray(b''.join(bytes.fromhex(n['code']) for n in nodes))
    # Windows x64 ABI: RCX=input span, RDX=12-byte output. R15/RDI/RBP saved.
    prologue=bytes.fromhex('415757554883ec204989cf4889d7')+b'\x48\x81\xef'+struct.pack('<I',output_disp)+bytes.fromhex('488d2c24')
    epilogue=bytes.fromhex('4883c4205d5f415fc3')
    literal=rip_load[2]
    instruction_offset=sum(n['size'] for n in nodes[:rip_load[0]])
    displacement=len(prologue)+len(original)+len(epilogue)-(len(prologue)+instruction_offset+nodes[rip_load[0]]['size'])
    original[instruction_offset+rip_load[1]:instruction_offset+rip_load[1]+4]=struct.pack('<i',displacement)
    blob=prologue+original+epilogue+literal
    return blob,{'schema':1,'scope':'original observed straight-line scalar slice on owned buffers only',
                 'source_span':batch['input_span'],'source_stride':batch['input_stride'],
                 'output_bytes':12,'source_offsets':sorted(source_loads),'literal_hex':literal.hex(),
                 'original_instruction_count':len(nodes),'original_code_sha256':hashlib.sha256(b''.join(bytes.fromhex(n['code']) for n in nodes)).hexdigest(),
                 'thunk_sha256':hashlib.sha256(blob).hexdigest(),'replacement_allowed':False,
                 'excluded':'callback input production, earlier branch, consumer, and frame scheduling'}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('packet_root',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();blob,meta=build(args.packet_root)
    args.output.mkdir(parents=True,exist_ok=False)
    (args.output/'original-slice.bin').write_bytes(blob)
    (args.output/'original-slice.json').write_text(json.dumps(meta,indent=2)+'\n')
    print(json.dumps({'bytes':len(blob),'instructions':meta['original_instruction_count'],'sha256':meta['thunk_sha256']}))


if __name__=='__main__':main()
