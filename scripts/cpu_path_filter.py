"""Choose an observational entry-mode filter from a witnessed branch divergence."""
import argparse,json
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_REG
from cpu_contract_events import event_records
from cpu_producer_plan import GPRS
from cpu_cfg_constants import canonical,NONVOLATILE

def build(positive,negative):
 def load(p):
  m=json.loads((p/'capture.json').read_text());e=list(event_records(p,m));end=next(i for i,x in enumerate(e) if x['kind']==2);return m,e[:end+1]
 pm,p=load(positive);nm,n=load(negative);d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True
 k=0
 while k<min(len(p),len(n)) and p[k]['rip']-pm['main_base']==n[k]['rip']-nm['main_base']:
  if p[k]['code']!=n[k]['code']:raise ValueError('code generation mismatch')
  k+=1
 if k<2:raise ValueError('no common branch prefix')
 branch=next(d.disasm(bytes.fromhex(p[k-1]['code']),p[k-1]['rip']))
 compare=next(d.disasm(bytes.fromhex(p[k-2]['code']),p[k-2]['rip']))
 if not branch.group(capstone.CS_GRP_JUMP) or compare.mnemonic not in ('cmp','test'):raise ValueError('divergence is not simple branch')
 written=set()
 for e in p[:k-2]:
  i=next(d.disasm(bytes.fromhex(e['code']),e['rip']))
  if i.group(capstone.CS_GRP_CALL):raise ValueError('call before filter; preservation unclosed')
  written.update(canonical(i.reg_name(x)) for x in i.regs_access()[1])
 for op in compare.operands:
  if op.type!=X86_OP_REG or op.size>4:continue
  name=compare.reg_name(op.reg);base=canonical(name)
  if base not in NONVOLATILE or base in written or name in ('ah','bh','ch','dh'):continue
  index=GPRS.index(base);mask=(1<<(op.size*8))-1;want=p[0]['registers'][index]&mask;other=n[0]['registers'][index]&mask
  if want!=other:
   return {'register_index':index,'register':base,'mask':mask,'value':want,'other_observed_value':other,'branch_rva':branch.address-pm['main_base'],'compare_rva':compare.address-pm['main_base'],'evidence':[str(positive),str(negative)],'scope':'training observation filter only; not replacement guard','selection':'first unchanged nonvolatile operand distinguishing witnessed paths'}
 raise ValueError('no stable discriminating entry register')
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('positive',type=Path);a.add_argument('negative',type=Path);a.add_argument('output',type=Path);x=a.parse_args();r=build(x.positive,x.negative);x.output.write_text(json.dumps(r,indent=2));print(r)
