"""Instruction-derived candidates from bounded live CPU samples; never an admission by sample alone."""
import argparse,bisect,csv,hashlib,json
from collections import Counter,defaultdict
from pathlib import Path,PureWindowsPath
import capstone,pefile
from capstone.x86 import X86_OP_MEM,X86_OP_REG,X86_OP_IMM
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
REGS={x:x for x in ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi']+[f'r{i}' for i in range(8,16)]}
def canonical(reg):
 if not reg:return None
 if reg in REGS:return reg
 if reg.startswith('e') and 'r'+reg[1:] in REGS:return 'r'+reg[1:]
 if reg.startswith('r') and reg[-1:] in ['d','w','b'] and reg[:-1] in REGS:return reg[:-1]
 return None

def record_exchanges(body):
 def memory(op):return (op.mem.base,op.mem.index,op.mem.scale,op.mem.disp,op.mem.segment,op.size)
 found=[]
 for a,b,c,d in zip(body,body[1:],body[2:],body[3:]):
  if not all(x.mnemonic.startswith(('mov','vmov')) and len(x.operands)==2 for x in [a,b,c,d]):continue
  aa,bb,cc,dd=[x.operands for x in [a,b,c,d]]
  if [x.type for x in aa+bb+cc+dd]!=[X86_OP_REG,X86_OP_MEM,X86_OP_REG,X86_OP_MEM,X86_OP_MEM,X86_OP_REG,X86_OP_MEM,X86_OP_REG]:continue
  if aa[0].reg==cc[1].reg and bb[0].reg==dd[1].reg and memory(aa[1])==memory(dd[0]) and memory(bb[1])==memory(cc[0]) and aa[1].size==bb[1].size:
   found.append({'pc':a.address,'bytes':aa[1].size,'meaning':'paired record exchange; not proof of whole sort contract'})
 return found

def describe_loop(insns,lo,hi):
 body=[i for i in insns if lo<=i.address<=hi];accesses=[];calls=[];interior_branches=[];unsupported=[];steps={};writes=Counter()
 for i in body:
  for reg in i.regs_access()[1]:
   n=canonical(i.reg_name(reg))
   if n:writes[n]+=1
  if i.group(capstone.CS_GRP_CALL):calls.append(i.address)
  if i.group(capstone.CS_GRP_JUMP) and i.address!=hi:interior_branches.append(i.address)
  if i.mnemonic.startswith(('lock','rep')) or i.mnemonic in ['syscall','cpuid','xsave','xrstor','rdtsc']:unsupported.append(i.address)
  if i.mnemonic in ['add','sub'] and len(i.operands)==2 and i.operands[0].type==X86_OP_REG and i.operands[1].type==X86_OP_IMM:
   n=canonical(i.reg_name(i.operands[0].reg));step=i.operands[1].imm*(1 if i.mnemonic=='add' else -1)
   if n:steps[n]=step
  if i.mnemonic in ['inc','dec'] and i.operands[0].type==X86_OP_REG:
   n=canonical(i.reg_name(i.operands[0].reg))
   if n:steps[n]=1 if i.mnemonic=='inc' else -1
  if i.mnemonic not in ('lea','nop'):
   for operand_index,op in enumerate(i.operands):
    if op.type!=X86_OP_MEM:continue
    read=bool(op.access&capstone.CS_AC_READ);write=bool(op.access&capstone.CS_AC_WRITE)
    # Capstone 5.0.6 marks VEX vmovdqa memory destinations as READ. Direction
    # of decoded x86 move operands is authoritative for this supported family.
    if i.mnemonic.startswith(('mov','vmov')):read=operand_index!=0;write=operand_index==0
    elif operand_index==0 and i.mnemonic in ['add','sub','and','or','xor','inc','dec','xadd','xchg','cmpxchg']:read=True;write=True
    accesses.append(dict(pc=i.address,address_size=i.addr_size,size=op.size,read=read,write=write,base=i.reg_name(op.mem.base),index=i.reg_name(op.mem.index),scale=op.mem.scale,displacement=op.mem.disp,segment=i.reg_name(op.mem.segment)))
 # Read-before-definition of a register also written in the loop is a
 # recurrence candidate, not an independent per-element temporary.
 defined=set();read_before=set();all_written=set()
 for i in body:
  rr,ww=i.regs_access();read_before.update(i.reg_name(x) for x in rr if i.reg_name(x) not in defined);all_written.update(i.reg_name(x) for x in ww);defined.update(i.reg_name(x) for x in ww)
 carried=sorted(read_before & all_written)
 reasons=[]
 if any(x.startswith(('xmm','ymm','zmm')) for x in carried):reasons.append('simd_loop_carried_state_requires_recurrence_semantics')
 if calls:reasons.append('calls_have_unclosed_effects_and_dependencies')
 if interior_branches:reasons.append('internal_control_flow_not_proven_elementwise')
 if unsupported:reasons.append('unsupported_atomic_or_system_effects')
 read=[a for a in accesses if a['read']];store=[a for a in accesses if a['write']]
 if not read:reasons.append('no_proven_input_stream')
 if not store:reasons.append('no_proven_output_stream')
 stream=[]
 for a in accesses:
  regs=[canonical(a['base']),canonical(a['index'])];mutable=[n for n in regs if n and writes[n]]
  affine=all(n in steps and writes[n]==1 for n in mutable)
  stride=sum(steps.get(n,0)*(a['scale'] if j==1 else 1) for j,n in enumerate(regs)) if affine else None
  stream.append(dict(**a,iteration_stride_candidate=stride))
 if any(x['iteration_stride_candidate'] is None for x in stream):reasons.append('memory_address_recurrence_not_proven_affine')
 if any(x['write'] and x['iteration_stride_candidate'] is not None and abs(x['iteration_stride_candidate'])<x['size'] for x in stream):reasons.append('output_overlap_between_iterations')
 # Deliberate safety gate: samples cannot establish complete invocation footprints.
 reasons += ['entry_bounds_and_task_size_unknown','cross_iteration_aliasing_unproven','concurrent_writer_ownership_unproven','first_consumer_and_deadline_unknown','register_flags_and_numeric_exit_contract_unproven']
 return dict(record_exchanges=record_exchanges(body),primitive_hint='conditional_record_exchange' if interior_branches and record_exchanges(body) else None,instructions=[dict(pc=i.address,bytes=i.bytes.hex(),mnemonic=i.mnemonic,operands=i.op_str) for i in body],memory_operands=stream,call_sites=calls,interior_branches=interior_branches,induction_candidates=steps,loop_carried_register_candidates=carried,classification='affine_map_shape' if read and store and 'output_overlap_between_iterations' not in reasons and all(x['iteration_stride_candidate'] is not None for x in stream) and not calls and not interior_branches and not unsupported and not any(x.startswith(('xmm','ymm','zmm')) for x in carried) else 'unsupported_or_unclosed_loop',admitted=False,reasons=reasons)

def analyze(root):
 with (root/'modules.csv').open(encoding='utf-8') as f:modules=list(csv.DictReader(f))
 main=next(m for m in modules if m['main']=='1');base=int(main['base'],16);image=root/'module-image.bin';pe=pefile.PE(str(image));
 if pe.FILE_HEADER.Machine!=0x8664:raise ValueError('Unsupported image architecture; x64 required')
 identity=hashlib.sha256(image.read_bytes()).hexdigest()
 bounds=sorted((int(x.struct.BeginAddress),int(x.struct.EndAddress)) for x in getattr(pe,'DIRECTORY_ENTRY_EXCEPTION',[]));starts=[x[0] for x in bounds]
 with (root/'samples.csv').open() as f:samples=list(csv.DictReader(f))
 with (root/'code-sections.csv').open() as f:sections=[dict(rva=int(x['rva']),data=(root/x['file']).read_bytes()) for x in csv.DictReader(f) if x['read_complete']=='1']
 def code(begin,end):
  for s in sections:
   if s['rva']<=begin<end<=s['rva']+len(s['data']):return s['data'][begin-s['rva']:end-s['rva']]
  return None
 grouped=defaultdict(list);unresolved=0;other=Counter()
 for x in samples:
  pc=int(x['rip'],16);rva=pc-base;idx=bisect.bisect_right(starts,rva)-1
  if idx>=0 and bounds[idx][0]<=rva<bounds[idx][1]:grouped[bounds[idx]].append(x)
  else:
   unresolved+=1
   for m in modules:
    if int(m['base'],16)<=pc<int(m['base'],16)+int(m['size']):other[PureWindowsPath(m['path']).name]+=1;break
 candidates=[];functions=[]
 for (begin,end),hits in sorted(grouped.items(),key=lambda x:len(x[1]),reverse=True)[:32]:
  entry=dict(module_sha256=identity,rva=begin,end_rva=end,samples=len(hits),tids=sorted({int(x['tid']) for x in hits}),call_frequency=None,cpu_duration_ms=None,critical_path_saving_ms=None)
  functions.append(entry)
  if end-begin>65536:entry['reason']='decode_budget_64KiB';continue
  blob=code(begin,end)
  if blob is None:entry['reason']='live_code_unavailable';continue
  if blob!=pe.get_data(begin,end-begin):entry['reason']='live_code_differs_from_disk_generation';continue
  insns=list(md.disasm(blob,begin));known={i.address for i in insns};loop_count=0
  for i in insns:
   if not i.group(capstone.CS_GRP_JUMP) or not i.operands or i.operands[0].type!=X86_OP_IMM:continue
   target=i.operands[0].imm
   if target not in known or target>=i.address or i.address-target>4096:continue
   observed=[x for x in hits if target<=int(x['rip'],16)-base<=i.address]
   if len(observed)<2:continue
   if len(candidates)>=64:break
   loop=describe_loop(insns,target,i.address);loop_count+=1
   # Register snapshots give prospective EAs, not proof of a completed access or range ownership.
   ea=[]
   for x in observed[:32]:
    rva=int(x['rip'],16)-base
    for op in loop['memory_operands']:
     if op['pc']!=rva or op['segment'] or op['address_size']!=8:continue
     b=canonical(op['base']);ind=canonical(op['index'])
     if (op['base'] and b is None) or (op['index'] and ind is None):continue
     addr=((int(x[b],16) if b else 0)+(int(x[ind],16) if ind else 0)*op['scale']+op['displacement'])&((1<<64)-1)
     ea.append(dict(tid=int(x['tid']),elapsed_ms=float(x['elapsed_ms']),pc=rva,address=addr,size=op['size'],read=op['read'],write=op['write'],kind='prospective_sampled_context_not_full_footprint'))
   candidates.append(dict(id=f'{identity[:16]}:{target:x}-{i.address:x}',function_rva=begin,loop_rva=target,backedge_rva=i.address,samples=len(observed),sampled_contexts=observed[:32],prospective_addresses=ea,cpu_cost_ms=None,task_size_distribution=None,complete_input_regions=None,complete_output_regions=None,first_consumers=None,transfer_cost_ms=None,gpu_cost_ms=None,synchronization_cost_ms=None,frame_saving_ms=None,**loop))
  entry['hot_loops']=loop_count
 candidates.sort(key=lambda x:x['samples'],reverse=True)
 result=dict(schema=1,decoder_version=capstone.__version__,budgets={'ranked_functions':32,'function_bytes':65536,'loop_bytes':4096,'loop_candidates':64},scope='automatic name-blind runtime sampling and machine-code loop/dependence screen; no replacement',profile=json.loads((root/'probe.json').read_text()),selection_uses_symbols=False,selection_uses_engine_source=False,ranking='sample-hotness triage only; critical path unknown, not summed thread time',module=main,module_sha256=identity,sample_count=len(samples),outside_resolved_main_functions=unresolved,other_modules=other,functions=functions,candidates=candidates,admitted=sum(x['admitted'] for x in candidates),replaced_cpu_invocations=0,transferred_bytes=0)
 (root/'candidates.json').write_text(json.dumps(result,indent=2))
 with (root/'candidate-table.csv').open('w',newline='') as f:
  writer=csv.writer(f);writer.writerow(['id','samples','classification','memory_operands','writes','admitted','reasons'])
  for c in candidates:writer.writerow([c['id'],c['samples'],c['classification'],len(c['memory_operands']),sum(x['write'] for x in c['memory_operands']),c['admitted'],';'.join(c['reasons'])])
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();j=analyze(a.directory);print(json.dumps({'samples':j['sample_count'],'functions':len(j['functions']),'candidates':len(j['candidates']),'admitted':j['admitted']}))
