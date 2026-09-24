"""Observed call contracts; trace facts are not universal replacement proofs."""
import argparse,csv,json,importlib.util,itertools,bisect
from cpu_contract_events import event_records
from cpu_state_liveness import Liveness
from pathlib import Path
from collections import Counter
import capstone
from capstone.x86 import X86_OP_MEM
spec=importlib.util.spec_from_file_location('discovery',Path(__file__).with_name('discover-cpu-tasks.py'));d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
REGS=['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi']+[f'r{i}' for i in range(8,16)]

class Intervals:
 def __init__(self):self.ranges=[];self.overflow=False
 def add(self,lo,hi):
  i=bisect.bisect_left(self.ranges,[lo,hi])
  if i and self.ranges[i-1][1]>=lo:i-=1
  while i<len(self.ranges) and self.ranges[i][0]<=hi:
   a,b=self.ranges.pop(i);lo=min(lo,a);hi=max(hi,b)
  if len(self.ranges)>=8192:self.overflow=True;return
  self.ranges.insert(i,[lo,hi])


def admission(properties):
 # A real proof/guard result is required for EVERY property; observations alone never authorize reuse.
 return all(p['status'] in ['proved_static','checked_each_invocation'] for p in properties.values())

def analyze(path):
 meta=json.loads((path/'capture.json').read_text());events=event_records(path,meta);boundary=meta.get('boundary_mode',False);consumer_mode=meta.get('consumer_mode',False)
 exit_index=None;after=None;memory_count=0;unsupported=[];calls=[];instruction_counts=Counter();flow_gaps=[];last_writes={};dependencies=set();reads=Intervals();writes=Intervals();first_consumer=None;live_reads=set();tail_defined=set();changed=[];tracking_truncated=False;cache={}
 liveness=Liveness()
 before=next(events,None)
 pure_path=path/'pure-instructions.json';pure={meta['main_base']+x['rva']:x['size'] for x in json.loads(pure_path.read_text())} if pure_path.exists() else {}
 pure_end={}
 def after_pure(pc):
  if pc in pure_end:return pure_end[pc]
  origin=pc
  for _ in range(len(pure)+1):
   if pc not in pure:break
   pc+=pure[pc]
  pure_end[origin]=pc;return pc
 memory_file=(path/'memory.csv').open('w',newline='');fields=['index','pc','address','size','read','write','kind','in_call'];memory_writer=csv.DictWriter(memory_file,fieldnames=fields);memory_writer.writeheader()
 def add(e,addr,size,read,write,kind):
  nonlocal first_consumer,memory_count,tracking_truncated
  if size<=0 or size>64 or addr+size>1<<64:unsupported.append({'index':e['index'],'reason':'unknown_or_large_memory_width'});return
  body=exit_index is None or e['index']<exit_index
  item={'index':e['index'],'pc':e['rip'],'address':addr,'size':size,'read':read,'write':write,'kind':kind,'in_call':body};memory_writer.writerow(item);memory_count+=1
  if body:
   if read:reads.add(addr,addr+size)
   if write:writes.add(addr,addr+size)
   for a in range(addr,addr+size):
    if read and a in last_writes:
     if len(dependencies)<8192:dependencies.add((last_writes[a],e['index']))
     else:tracking_truncated=True
    if write:
     if a in last_writes or len(last_writes)<262144:last_writes[a]=e['index']
     else:tracking_truncated=True
  elif read and first_consumer is None and (any(a in last_writes for a in range(addr,addr+size)) or (consumer_mode and meta.get('snapshot_after') and addr<meta['snapshot_begin']+meta['snapshot_bytes'] and meta['snapshot_begin']<addr+size)):
   if not(meta.get('stack_low',0)<=addr<meta.get('stack_high',0)):first_consumer=item
 if not boundary or consumer_mode:
  for e,nxt in itertools.pairwise(itertools.chain([before],events)):
   if nxt['kind']==2:after=nxt;exit_index=nxt['index']
   if len(unsupported)>4096:unsupported=unsupported[:4096];tracking_truncated=True
   if len(calls)>8192:calls=calls[:8192];tracking_truncated=True
   if len(flow_gaps)>4096:flow_gaps=flow_gaps[:4096];tracking_truncated=True
   if consumer_mode and e['kind']==1:continue
   key=(e['rip'],e['code']);ins=cache.get(key)
   if ins is None:
    ins=list(d.md.disasm(bytes.fromhex(e['code']),e['rip'],count=1))
    if len(cache)<4096:cache[key]=ins
   if not ins:unsupported.append({'index':e['index'],'reason':'decode_unavailable'});continue
   i=ins[0];instruction_counts[i.mnemonic]+=1;registers=dict(zip(REGS,e['registers']))
   if not i.group(capstone.CS_GRP_JUMP) and not i.group(capstone.CS_GRP_CALL) and not i.group(capstone.CS_GRP_RET) and i.mnemonic not in ['syscall','sysenter'] and nxt['rip']!=after_pure(i.address+i.size):flow_gaps.append(e['index'])
   if i.group(capstone.CS_GRP_CALL):calls.append({'index':e['index'],'pc':i.address,'observed_target':nxt['rip']})
   if i.mnemonic.startswith(('rep','xsave','xrstor','vgather','vscatter','vmaskmov')) or i.mnemonic in ['syscall','sysenter','int','iret','iretq']:
    unsupported.append({'index':e['index'],'reason':'implicit_or_kernel_memory_effects','instruction':i.mnemonic})
   for k,op in enumerate(i.operands):
    if op.type!=X86_OP_MEM or i.mnemonic=='lea':continue
    def value(reg):
     if not reg:return 0
     name=i.reg_name(reg)
     if name=='rip':return i.address+i.size
     c=d.canonical(name)
     if c is None:raise ValueError('vector_or_unknown_address_register')
     return registers[c]
    try:
     addr=(value(op.mem.base)+value(op.mem.index)*op.mem.scale+op.mem.disp)&((1<<(i.addr_size*8))-1)
     seg=i.reg_name(op.mem.segment)
     if seg=='gs':
      if not e.get('teb'):raise ValueError('segment_base_not_captured')
      addr+=e['teb']
     elif seg:raise ValueError('unsupported_segment')
    except ValueError as ex:unsupported.append({'index':e['index'],'reason':str(ex)});continue
    read=bool(op.access&capstone.CS_AC_READ);write=bool(op.access&capstone.CS_AC_WRITE)
    if i.mnemonic.startswith(('mov','vmov')):read=k!=0;write=k==0
    elif k==0 and i.mnemonic.removeprefix('lock ') in ['add','sub','and','or','xor','inc','dec','xadd','xchg','cmpxchg']:read=write=True
    if not read and not write:unsupported.append({'index':e['index'],'reason':'unknown_memory_direction'})
    add(e,addr,op.size,read,write,'explicit')
   if i.group(capstone.CS_GRP_CALL) or i.mnemonic in ['push','pushfq']:add(e,registers['rsp']-8,8,False,True,'implicit_stack')
   if i.group(capstone.CS_GRP_RET) or i.mnemonic in ['pop','popfq']:add(e,registers['rsp'],8,True,False,'implicit_stack')
   if i.mnemonic=='leave':add(e,registers['rbp'],8,True,False,'implicit_stack')
   if exit_index is not None and e['index']>=exit_index:
    liveness.add(i)
 else:
  for event in events:
   if event['kind']==2:after=event;exit_index=event['index']
 memory_file.close()
 if before and after:changed=[r for k,r in enumerate(REGS) if before['registers'][k]!=after['registers'][k]]
 tracking_truncated=tracking_truncated or reads.overflow or writes.overflow
 body_unsupported=[u for u in unsupported if exit_index is None or u['index']<exit_index]
 body_gaps=[g for g in flow_gaps if exit_index is None or g<exit_index]
 observed_footprint_complete=not boundary and exit_index is not None and not body_unsupported and not body_gaps and not reads.overflow and not writes.overflow and (not meta.get('streaming') or meta.get('written_events')==meta['events'])
 properties={
 'code_identity':{'status':'checked_study_only','evidence':'PE timestamp/size and exact function bytes checked before arming'},
 'whole_call':{'status':'observed' if meta['completed_calls'] else 'unknown','evidence':'entry RIP, saved return address and exact return RSP' if meta['completed_calls'] else 'capture limit reached before matching return'},
 'all_paths_and_exits':{'status':'unknown','evidence':'one dynamic path does not prove all exits/callee effects'},
 'memory_footprint':{'status':'observed' if observed_footprint_complete else 'unknown','evidence':{'memory_events':memory_count,'unresolved_effects':len(body_unsupported),'flow_discontinuities':len(body_gaps)}},
 'cross_iteration_dependencies':{'status':'unknown','evidence':{'observed_read_after_write_pairs':len(dependencies),'not_absence_proof':True}},
 'external_thread_ownership':{'status':'unknown','evidence':{'concurrent_function_entries':meta['concurrent_entry_hits'],'other_thread_memory_observed':False}},
 'first_global_consumer':{'status':'unknown','evidence':{'first_observed_same_thread_non_stack_read':first_consumer,'tail_instruction_budget':meta.get('tail_instruction_limit',256)}},
 'exit_state_and_numeric_semantics':{'status':'unknown','evidence':{'changed_gprs':changed,'observed_caller_required_output_masks':liveness.result(),'flags_changed':before['eflags']!=after['eflags'] if before and after else None,'mxcsr_entry':before['mxcsr'] if before else None,'mxcsr_exit':after['mxcsr'] if after else None,'reported_xstate_components_captured':bool(meta.get('xstate_before_ok') and meta.get('xstate_after_ok'))}},
 'per_invocation_guards':{'status':'unknown','evidence':'no executable memory/ownership guards have been established for replacement'},
 'profitable_replacement':{'status':'unknown','evidence':'original computation still runs; no GPU trial authorized'}
 }
 output_ranges=writes.ranges;non_stack=[x for x in output_ranges if not(meta.get('stack_low',0)<=x[0]<meta.get('stack_high',0))]
 hypotheses=[]
 if before:
  registers=dict(zip(REGS,before['registers']))
  for lo_name in ['rcx','rdx','r8','r9']:
   for hi_name in ['rcx','rdx','r8','r9']:
    lo,hi=registers[lo_name],registers[hi_name]
    if lo<65536 or not 0<hi-lo<=16*1024*1024:continue
    covered=sum(max(0,min(b,hi)-max(a,lo)) for a,b in non_stack)
    if covered:hypotheses.append({'start_register':lo_name,'end_register':hi_name,'start':lo,'end':hi,'bytes':hi-lo,'observed_write_bytes_inside':covered,'possible_16_byte_records':(hi-lo)//16 if (hi-lo)%16==0 else None,'status':'hypothesis_requires_static_proof_and_per_call_guards'})
 result={'schema':1,'observed_footprint_complete':observed_footprint_complete,'argument_range_hypotheses':hypotheses,'admitted':admission(properties),'properties':properties,'boundary_only':boundary,'entry':before,'exit':after,'observed_input_ranges':reads.ranges,'observed_output_ranges':output_ranges,'non_stack_output_candidates':non_stack,'dataset_size':None,'read_after_write_pairs':sorted(dependencies),'calls':calls,'unsupported':unsupported,'flow_gaps':flow_gaps,'instruction_counts':instruction_counts,'memory_events':memory_count,'analysis_tracking_truncated':tracking_truncated,'analysis_budgets':{'intervals':8192,'dependency_addresses':262144,'dependency_edges':8192,'cached_pcs':4096},'cost':{'entry_to_return_ms':(meta['exit_qpc']-meta['start_qpc'])*1000/meta['qpc_frequency'] if meta['completed_calls'] else None,'handler_total_ms':meta['handler_ticks']*1000/meta['qpc_frequency'],'pure_cpu_ms':None,'wait_ms':None,'note':'wall includes debug delivery; handler time excludes OS dispatch and may include caller tail; GetThreadTimes zero may be below timer resolution','instrumented_thread_cpu_100ns':meta['instrumented_thread_cpu_100ns']},'trap_cleanup_pending':meta['trap_cleanup_pending']}
 (path/'contract.json').write_text(json.dumps(result,indent=2))
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();paths=sorted(a.directory.glob('candidate-*')) if (a.directory/'selection.json').exists() else [a.directory]
 for path in paths:
  if not (path/'capture.json').exists():continue
  r=analyze(path);print(path.name,'complete',bool(r['exit']),'memory',r['memory_events'],'unresolved',len(r['unsupported']),'admitted',r['admitted'])
