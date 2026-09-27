"""Cost-scoped CPU chain survey. Observed loop time never authorizes replacement."""
import argparse,json,statistics,sys
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_IMM,X86_OP_REG
from cpu_producer_plan import GPRS
from cpu_batch_plan import canonical
from cpu_contract_events import event_records

def distribution(values):
 if not values:return None
 v=sorted(values)
 return {'n':len(v),'mean':statistics.mean(v),'median':statistics.median(v),'min':v[0],'p95':v[int((len(v)-1)*.95)],'max':v[-1]}

def boundaries(folder):
 selection=json.loads((folder/'selection.json').read_text());result=[]
 for item in selection['candidates']:
  d=folder/item['directory']
  if not (d/'capture.json').exists():continue
  m=json.loads((d/'capture.json').read_text());events=list(event_records(d,m));costs=json.loads((d/'boundary-costs.json').read_text()) if (d/'boundary-costs.json').exists() else []
  if m['reason'] or m['trap_cleanup_pending'] or not m['boundary_mode']:raise ValueError('incomplete boundary measurement')
  if len(events)!=2*len(costs) or len(costs)!=m['completed_calls']:raise ValueError('boundary pairing mismatch')
  stride=None;index=None
  if m.get('region_mode'):
   decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
   ins=list(decoder.disasm((d/'expected-code.bin').read_bytes(),item['entry_rva']))
   if len(ins)>=3:
    add,compare,branch=ins[-3:]
    if add.mnemonic=='add' and add.operands[0].type==X86_OP_REG and add.operands[1].type==X86_OP_IMM and compare.mnemonic=='cmp' and compare.operands[0].type==X86_OP_REG and branch.mnemonic=='jne' and branch.operands[0].imm==item['entry_rva']:
     reg=canonical(add.reg_name(add.operands[0].reg))
     if reg==canonical(compare.reg_name(compare.operands[0].reg)) and reg in GPRS and add.operands[1].imm>0:index=GPRS.index(reg);stride=add.operands[1].imm
  seen=set();wall=[];handlers=[];counts=[];discarded=0
  for k,cost in enumerate(costs):
   a,b=events[2*k:2*k+2]
   if a['kind']!=1 or b['kind']!=2 or a['tid']!=b['tid'] or cost['tid']!=a['tid'] or cost['end_qpc']<cost['begin_qpc']:raise ValueError('invalid boundary pair')
   # Attaching at an inner header may see a partial first traversal per thread.
   first=a['tid'] not in seen;seen.add(a['tid'])
   if first and m.get('region_mode'):discarded+=1;continue
   wall.append((cost['end_qpc']-cost['begin_qpc'])*1000/m['qpc_frequency']);handlers.append(cost['handler_ticks_inside']*1000/m['qpc_frequency'])
   if index is not None:
    delta=b['registers'][index]-a['registers'][index]
    if delta>0 and delta%stride==0:counts.append(delta//stride)
  result.append({'candidate_ids':item['ids'],'entry_rva':item['entry_rva'],'scope':'observed_loop_including_callees_and_traps' if m.get('region_mode') else 'surrounding_function_including_callees_and_traps','wall_ms':distribution(wall),'known_handler_ms':distribution(handlers),'observed_elements':distribution(counts),'element_count_basis':{'stride':stride,'index_register':GPRS[index] if index is not None else None,'status':'boundary delta with decoded add/cmp/backedge; callee preservation not independently certified'},'discarded_first_partial_per_thread':discarded,'calls_per_frame':None,'frequency_note':'sampled owners; concurrent unobserved calls prohibit global invocation frequency','removable_cpu_ms':None,'critical_path_frame_ms':None,'source':str(d)})
 return result

def trace_facts(folder):
 m=json.loads((folder/'capture.json').read_text());events=list(event_records(folder,m,vectors=True));decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
 ops={};calls=[];unknown=[];reads=0;failed=0
 for e in events[:-1]:
  ins=next(decoder.disasm(bytes.fromhex(e['code']),e['rip']),None)
  if ins is None:raise ValueError('trace instruction decode')
  ops[ins.mnemonic]=ops.get(ins.mnemonic,0)+1
  if ins.group(capstone.CS_GRP_CALL):calls.append({'rva':ins.address-m['main_base'],'target_rva':ins.operands[0].imm-m['main_base'] if ins.operands[0].type==X86_OP_IMM else None})
  if not e.get('memory_known'):unknown.append(ins.address-m['main_base'])
  reads+=len(e.get('memory',[]));failed+=sum(not x['ok'] for x in e.get('memory',[]))
 return {'source':str(folder),'events':len(events),'one_traversal_completed':m['completed_calls']==1 and not m['trap_cleanup_pending'] and m['reason']==0,'memory_operands_captured':reads,'unmapped_instructions':unknown,'failed_memory_reads':failed,'operations':ops,'observed_calls':calls,'replacement_allowed':False,'contract':{'observed':['executed path, registers and memory values','normal return to caller' if m.get('callee_mode') else 'normal traversal back to header','instruction effects only on this sampled path'],'static':['byte-verified code and bounded memory operand plans'],'enforced':['event/time/memory limits','original instructions remain executed','instrumentation disabled before normal continuation'],'unknown':['other branches and arithmetic inputs','whole-batch inter-iteration state algebra','exclusive access, allocation generations and pointer escapes','final consumer/publication deadline','complete GPU lowering and eligibility guards']}}

def main():
 p=argparse.ArgumentParser();p.add_argument('boundary_root',type=Path);p.add_argument('output',type=Path);p.add_argument('--traces',type=Path,nargs='*',default=[]);a=p.parse_args()
 result={'schema':1,'boundaries':boundaries(a.boundary_root),'traces':[trace_facts(f) for f in a.traces],'replacement_allowed':False}
 a.output.write_text(json.dumps(result,indent=2)+'\n');print([(r['scope'],r['wall_ms'],r['observed_elements']) for r in result['boundaries']])
if __name__=='__main__':main()
