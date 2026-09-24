"""Structural audit of the byte-verified call closure; it does not assume sort semantics."""
import argparse,json,struct,hashlib
from pathlib import Path
import capstone
from capstone.x86 import X86_OP_MEM,X86_OP_IMM
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();blob=(a.directory/'checked-functions.bin').read_bytes();count=struct.unpack_from('<I',blob)[0];offset=4;functions=[]
d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True
for _ in range(count):
 rva,size=struct.unpack_from('<II',blob,offset);offset+=8;code=blob[offset:offset+size];offset+=size;functions.append((rva,code,list(d.disasm(code,rva))))
starts={r for r,_,_ in functions};calls=[];unclosed=[];atomics=[];system=[];global_writes=[];indirect_jumps=[];returns=[]
for r,code,instructions in functions:
 for i in instructions:
  if i.group(capstone.CS_GRP_CALL):
   target=i.operands[0].imm if i.operands and i.operands[0].type==X86_OP_IMM else None;edge={'rva':i.address,'target':target,'instruction':i.mnemonic+' '+i.op_str};calls.append(edge)
   if target not in starts:unclosed.append(edge)
  if i.group(capstone.CS_GRP_JUMP) and (not i.operands or i.operands[0].type!=X86_OP_IMM):indirect_jumps.append({'rva':i.address,'instruction':i.op_str})
  if i.mnemonic.startswith('lock ') or i.mnemonic=='xchg' and any(o.type==X86_OP_MEM for o in i.operands):atomics.append(i.address)
  if i.mnemonic in ['syscall','sysenter','rdtsc','rdtscp','cpuid','int']:system.append(i.address)
  if i.operands and i.operands[0].type==X86_OP_MEM and i.reg_name(i.operands[0].mem.base)=='rip' and (i.operands[0].access&capstone.CS_AC_WRITE or i.mnemonic.startswith(('mov','vmov'))):global_writes.append(i.address)
  if i.group(capstone.CS_GRP_RET):returns.append(i.address)
result={'checked_functions':[{ 'rva':r,'bytes':len(c),'sha256':hashlib.sha256(c).hexdigest()} for r,c,_ in functions],'direct_calls':calls,'unclosed_calls':unclosed,'indirect_jumps':indirect_jumps,'atomic_sites':atomics,'system_or_clock_sites':system,'rip_relative_store_sites':global_writes,'return_sites':returns,'all_paths_closed':not unclosed and not indirect_jumps,'proof_limits':['Function byte coverage does not prove memory bounds or pointer ownership.','Indirect control-flow destinations require separate proof.','Key comparisons and observed sorted output do not prove whole-algorithm equivalence.'],'admitted':False}
(a.directory/'static-audit.json').write_text(json.dumps(result,indent=2));print('functions',len(functions),'unclosed calls',len(unclosed),'indirect jumps',len(indirect_jumps),'atomics',len(atomics))
