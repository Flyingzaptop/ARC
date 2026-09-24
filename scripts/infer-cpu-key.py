"""Exact bit-projection inference for straight-line record comparisons; not a whole-sort proof."""
import json,struct,collections,importlib.util
from pathlib import Path
import capstone
from capstone.x86 import X86_OP_REG,X86_OP_MEM,X86_OP_IMM
spec=importlib.util.spec_from_file_location('d',Path(__file__).with_name('discover-cpu-tasks.py'));d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
ZERO=frozenset();ONE=frozenset([frozenset()])
def const(v):return [ONE if (v>>i)&1 else ZERO for i in range(64)]
def band(a,b):
 if len(a)*len(b)>128:raise ValueError("symbolic polynomial budget")
 out=set()
 for x in a:
  for y in b:
   z=x|y
   if len(z)>16:raise ValueError("symbolic degree budget")
   if z in out:out.remove(z)
   else:out.add(z)
 if len(out)>32:raise ValueError("symbolic polynomial budget")
 return frozenset(out)
def boolop(op,a,b):
 if op=='xor':return a^b
 if op=='and':return band(a,b)
 return a^b^band(a,b)
def projection(v):
 if v is None:return None
 bases=set();out=[]
 for bit in v:
  if not bit:out.append(-1);continue
  if bit==ONE:out.append(-2);continue
  if len(bit)!=1:return None
  mon=next(iter(bit))
  if len(mon)!=1:return None
  base,offset=next(iter(mon));bases.add(base);out.append(offset)
 if len(bases)!=1:return None
 return out

def infer(path):
 blob=(path/'checked-functions.bin').read_bytes();n=struct.unpack_from('<I',blob)[0];offset=4;sites=[];unmatched=[]
 for _ in range(n):
  rva,size=struct.unpack_from('<II',blob,offset);offset+=8;ins=list(d.md.disasm(blob[offset:offset+size],rva));offset+=size
  setters=collections.defaultdict(set)
  for i in ins:
   if i.mnemonic in ['mov','movabs'] and len(i.operands)==2 and i.operands[0].type==X86_OP_REG and i.operands[1].type==X86_OP_IMM:
    reg=d.canonical(i.reg_name(i.operands[0].reg))
    if reg in ['rbx','rbp','rsi','rdi','r12','r13','r14','r15']:setters[reg].add(i.operands[1].imm)
  constants={k:const(next(iter(v))) for k,v in setters.items() if len(v)==1};state=dict(constants);versions=collections.Counter()
  def read(op):
   if op.type==X86_OP_IMM:return const(op.imm)
   if op.type==X86_OP_REG:
    reg=d.canonical(i.reg_name(op.reg));value=state.get(reg)
    if value is None:return None
    return value[:op.size*8]+[ZERO]*max(0,64-op.size*8)
   if op.type==X86_OP_MEM:
    name=i.reg_name(op.mem.base);reg=d.canonical(name)
    if not reg or reg in ['rsp','rbp'] or op.mem.index or op.mem.segment or op.mem.disp<0 or op.mem.disp+op.size>16:return None
    base=(reg,versions[reg]);return [frozenset([frozenset([(base,op.mem.disp*8+k)])]) for k in range(op.size*8)]+[ZERO]*(64-op.size*8)
   return None
  for i in ins:
   ops=i.operands
   if i.mnemonic=='cmp' and len(ops)==2:
    a,b=read(ops[0]),read(ops[1]);pa,pb=projection(a),projection(b)
    if pa is not None and pa==pb:sites.append({'rva':i.address,'projection':pa,'constant_assumption':'unique nonvolatile literal setters plus Win64 call preservation; dominance not yet proven'})
    elif pa is not None or pb is not None:unmatched.append(i.address)
   result=None;dest=None
   if ops and ops[0].type==X86_OP_REG:dest=d.canonical(i.reg_name(ops[0].reg))
   if dest and len(ops)==2:
    if i.mnemonic in ['mov','movabs','movzx']:result=read(ops[1])
    elif i.mnemonic in ['and','or','xor']:
     a,b=read(ops[0]),read(ops[1])
     if a is not None and b is not None:result=[boolop(i.mnemonic,x,y) for x,y in zip(a,b)]
    elif i.mnemonic in ['shl','sal','shr'] and ops[1].type==X86_OP_IMM:
     a=read(ops[0]);shift=ops[1].imm&(63 if ops[0].size==8 else 31)
     if a is not None:result=(([ZERO]*shift+a)[:64] if i.mnemonic in ['shl','sal'] else (a[shift:]+[ZERO]*shift))
   if dest and i.mnemonic not in ['cmp','test']:
    if result is not None and ops[0].size<8:result=result[:ops[0].size*8]+[ZERO]*(64-ops[0].size*8)
    state[dest]=result;versions[dest]+=1
   if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_RET):state=dict(constants);versions=collections.Counter()
 counts=collections.Counter(tuple(x['projection']) for x in sites);checks=[]
 for bits,count in counts.most_common():
  check={'projection':bits,'sites':count}
  if (path/'input-span.bin').exists() and (path/'output-span.bin').exists():
   before=(path/'input-span.bin').read_bytes();after=(path/'output-span.bin').read_bytes()
   if len(before)%16==0 and len(before)==len(after):
    a=[before[i:i+16] for i in range(0,len(before),16)];b=[after[i:i+16] for i in range(0,len(after),16)]
    def key(record):
     x=int.from_bytes(record,'little');return sum(((1 if bit==-2 else 0 if bit==-1 else (x>>bit)&1)<<i) for i,bit in enumerate(bits))
    keys=[key(x) for x in b];check.update(records=len(b),same_record_multiset=collections.Counter(a)==collections.Counter(b),nondecreasing=all(x<=y for x,y in zip(keys,keys[1:])),distinct_keys=len(set(keys)),unique_keys=len(set(keys))==len(keys))
  checks.append(check)
 result={'scope':'instruction-derived exact local bit projection under recorded constant assumptions; snapshot tests are observations, not general algorithm or ownership proof','sites':sites,'unmatched_data_comparisons':unmatched,'hypotheses':checks,'admitted':False}
 (path/'key-hypotheses.json').write_text(json.dumps(result,indent=2));return result
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();j=infer(a.directory);print([(x['sites'],x.get('records'),x.get('same_record_multiset'),x.get('nondecreasing'),x.get('distinct_keys')) for x in j['hypotheses']])
