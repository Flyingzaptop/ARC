"""Conservative observed-continuation liveness with x64 register alias masks."""
import re
import capstone.x86 as x
FULL=(1<<64)-1
NAMES={'a':'rax','b':'rbx','c':'rcx','d':'rdx','si':'rsi','di':'rdi','bp':'rbp','sp':'rsp'}
def physical(name,write=False,vector=False):
 if name in ['rflags','eflags']:return 'flags',FULL
 if name in ['rip','eip','cs','ds','es','fs','gs','ss']:return None,0
 m=re.fullmatch(r'(xmm|ymm|zmm)(\d+)',name or '')
 if m:
  bits={'xmm':128,'ymm':256,'zmm':512}[m[1]]
  if write and vector and bits==128:bits=256
  return 'vec'+m[2],(1<<bits)-1
 m=re.fullmatch(r'r(\d+)([dwb]?)',name or '')
 if m:
  bits={'':64,'d':32,'w':16,'b':8}[m[2]]
  return 'r'+m[1],FULL if write and bits==32 else (1<<bits)-1
 for stem,reg in NAMES.items():
  if name==reg:return reg,FULL
  if name=='e'+reg[1:]:return reg,FULL if write else (1<<32)-1
  if name==stem+'x' and len(stem)==1 or name==stem and len(stem)>1:return reg,(1<<16)-1
  if len(stem)==1 and name==stem+'l' or len(stem)>1 and name==stem+'l':return reg,255
  if len(stem)==1 and name==stem+'h':return reg,255<<8
 return None,0

def accesses(ins):
 rr,ww=ins.regs_access();reads={};writes={}
 for reg in rr:
  key,mask=physical(ins.reg_name(reg))
  if key:reads[key]=reads.get(key,0)|mask
 for reg in ww:
  key,mask=physical(ins.reg_name(reg),True,ins.mnemonic.startswith('v'))
  if key:writes[key]=writes.get(key,0)|mask
 ops=ins.operands
 if ins.mnemonic in ['xor','sub','pxor','xorps','xorpd'] and len(ops)==2 and all(o.type==x.X86_OP_REG for o in ops) and ops[0].reg==ops[1].reg:
  reads.pop(physical(ins.reg_name(ops[0].reg))[0],None)
 if ins.mnemonic in ['vpxor','vxorps','vxorpd'] and len(ops)==3 and all(o.type==x.X86_OP_REG for o in ops) and ops[1].reg==ops[2].reg:reads.pop(physical(ins.reg_name(ops[1].reg))[0],None)
 flag_reads=flag_writes=0
 for flag,bit in [('CF',0),('PF',2),('AF',4),('ZF',6),('SF',7),('TF',8),('IF',9),('DF',10),('OF',11)]:
  for kind in ['TEST','PRIOR']:
   if ins.eflags&getattr(x,'X86_EFLAGS_'+kind+'_'+flag,0):flag_reads|=1<<bit
  for kind in ['MODIFY','RESET','SET']:
   if ins.eflags&getattr(x,'X86_EFLAGS_'+kind+'_'+flag,0):flag_writes|=1<<bit
 if 'flags' in reads and flag_reads:reads['flags']=flag_reads
 if 'flags' in writes:writes['flags']=flag_writes
 if ins.mnemonic=='vzeroupper':
  for i in range(16):writes['vec'+str(i)]=((1<<256)-1)^((1<<128)-1)
 return reads,writes
class Liveness:
 def __init__(self):self.defined={};self.needed={}
 def add(self,ins):
  rr,ww=accesses(ins)
  for reg,mask in rr.items():self.needed[reg]=self.needed.get(reg,0)|(mask&~self.defined.get(reg,0))
  for reg,mask in ww.items():self.defined[reg]=self.defined.get(reg,0)|mask
 def result(self):return {k:hex(v) for k,v in self.needed.items() if v}
