"""Select whole entry regions exclusively from detector output; no symbols/game offsets."""
import argparse,hashlib,json
from pathlib import Path
import pefile,capstone,struct
from cpu_parallel_screen import build as parallel_screen
from capstone.x86 import X86_OP_MEM,X86_OP_IMM
PURE={"mov","movzx","movsx","movsxd","lea","add","sub","and","or","xor","shl","shr","sar","sal","inc","dec","cmp","test","neg","not","pxor"}
def pure_instruction(i):return i.mnemonic in PURE and (i.mnemonic=="lea" or not any(x.type==X86_OP_MEM for x in i.operands))

def select(probe,output,mode="trace",events=32768,ms=100,max_span=0,outermost=False,stop_unsupported=False,context=None,measurement_path=None,boundary_samples=1,loop_regions=False,iteration_skip=0,callee_from=None,iteration_samples=1,entry_filter=None,view_budget_mib=128,pointer_target_plan=None,defer_first_region=False):
 if not 16<=view_budget_mib<=256:raise ValueError('view snapshot budget')
 if not 1<=iteration_samples<=128:raise ValueError('iteration sample budget')
 if mode in ('iteration','bulk') and not loop_regions:raise ValueError('iteration mode requires loop boundaries')
 if loop_regions and mode not in ('boundary','iteration','bulk'):raise ValueError('incompatible region mode')
 if (mode=='callee') != (callee_from is not None):raise ValueError('callee mode requires observed call evidence')
 if boundary_samples*2>events:raise ValueError('event budget too small for boundary samples')
 if not 0<=iteration_skip<=1024:raise ValueError('iteration sampling budget')
 if not 1<=boundary_samples<=32:raise ValueError('boundary sample budget')
 j=json.loads((probe/'candidates.json').read_text());image=probe/'module-image.bin';pe=pefile.PE(str(image));digest=hashlib.sha256(image.read_bytes()).hexdigest()
 if digest!=j['module_sha256']:raise ValueError('Image generation mismatch')
 raw_bounds={int(x.struct.BeginAddress):x.struct for x in pe.DIRECTORY_ENTRY_EXCEPTION}
 def ultimate(fn):
  for _ in range(16):
   info=pe.get_data(fn.UnwindData,4)
   if not info or not(info[0]>>3)&4:return fn.BeginAddress
   begin,end,unwind=struct.unpack('<III',pe.get_data(fn.UnwindData+4+((info[2]+1)//2)*4,12));fn=type('Bound',(),dict(BeginAddress=begin,EndAddress=end,UnwindData=unwind))
  raise ValueError('Cyclic/deep chained unwind metadata')
 grouped={};owner={}
 for start,fn in raw_bounds.items():
  root=ultimate(fn);grouped.setdefault(root,[]).append((fn.BeginAddress,fn.EndAddress));owner[start]=root
 bounds={}
 for root,ranges in grouped.items():
  ranges.sort();contiguous=all(a[1]==b[0] for a,b in zip(ranges,ranges[1:]))
  if contiguous:bounds[root]=type('Bound',(),dict(BeginAddress=root,EndAddress=ranges[-1][1],UnwindData=raw_bounds[root].UnwindData,parts=ranges))

 groups={}
 measurements_path=measurement_path or probe/'parallel-measurements.json'
 measurements=json.loads(measurements_path.read_text()) if measurements_path.exists() else {}
 if measurements and measurements['module_sha256']!=digest:raise ValueError('Measurement generation mismatch')
 ranking=parallel_screen(probe,context,measurements=measurements.get('candidates',{}),conditions=measurements.get('current_conditions',{}))
 ranks={c['id']:c for c in ranking['candidates']}
 for c in ranking['candidates']:
  if not (c['bounded_research_eligible'] or c['cheap_measurement_eligible']):continue
  f=c['function_rva'];g=groups.setdefault(f,{'samples':0,'ids':[],'exchange':False,'screen_rank':len(ranks)+1,'deep_eligible':False,'cheap_eligible':False});g['samples']=max(g['samples'],c['sample_hits']);g['ids'].append(c['id']);g['exchange']|=c['topology']=='ordered_exchange';g['screen_rank']=min(g['screen_rank'],ranks[c['id']]['rank']);g['deep_eligible']|=ranks[c['id']]['detailed_capture_eligible'];g['cheap_eligible']|=ranks[c['id']]['bounded_research_eligible'] or ranks[c['id']]['cheap_measurement_eligible']
 if callee_from is not None:
  from cpu_contract_events import event_records
  inherited=json.loads((callee_from.parent/'selection.json').read_text())
  if inherited['image_sha256']!=digest:raise ValueError('callee observation generation mismatch')
  parent=next(c for c in inherited['candidates'] if c['directory']==callee_from.name)
  if all(ranks[k]['economics_status']=='unprofitable_or_not_recurring_large_batch' for k in parent['ids']):raise ValueError('parent route rejected economically in current context')
  meta=json.loads((callee_from/'capture.json').read_text());decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True;targets=[]
  for event in event_records(callee_from,meta):
   ins=next(decoder.disasm(bytes.fromhex(event['code']),event['rip']),None)
   if ins and ins.group(capstone.CS_GRP_CALL) and ins.operands[0].type==X86_OP_IMM:
    rva=ins.address-meta['main_base']
    if pe.get_data(rva,ins.size)!=ins.bytes:raise ValueError('observed call bytes changed')
    target=ins.operands[0].imm-meta['main_base']
    if target in owner and target not in targets:targets.append(target)
  groups={target:{'samples':1,'ids':parent['ids'],'exchange':False,'screen_rank':k,'deep_eligible':True,'cheap_eligible':True,'seed_origin':'direct_call_in_automatically_selected_iteration'} for k,target in enumerate(targets[:4])}
 output.mkdir(parents=True,exist_ok=True);plan=[]
 (output/'parallel-screen.json').write_text(json.dumps(ranking,indent=2))
 for f,g in sorted(groups.items(),key=lambda x:(x[1]['screen_rank'],x[0])):
  if not g['cheap_eligible']:continue
  logical=owner.get(f,f)
  if logical not in bounds:continue # unknown split layout stays unselected, never guessed
  b=bounds[logical];chains=[]
  for _ in range(8):
   raw=pe.get_data(b.UnwindData,4)
   if not raw or not (raw[0]>>3)&4:break
   offset=b.UnwindData+4+((raw[2]+1)//2)*4
   begin,end,unwind=struct.unpack('<III',pe.get_data(offset,12));chains.append(f);b=type('Bound',(),dict(BeginAddress=begin,EndAddress=end,UnwindData=unwind))
  if loop_regions:
   chosen=min((c for c in ranking['candidates'] if c['id'] in g['ids']),key=lambda c:c['rank'])
   start,end=chosen['loop_rva'],chosen['backedge_rva']
   decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
   last=next(decoder.disasm(pe.get_data(end,15),end),None)
   if last is None:continue
   end+=last.size;region=list(decoder.disasm(pe.get_data(start,end-start),start))
   exits=[]
   for ins in region:
    if ins.group(capstone.CS_GRP_RET):exits.append(None)
    if ins.group(capstone.CS_GRP_JUMP):
     target=ins.operands[0].imm if ins.operands and ins.operands[0].type==X86_OP_IMM else None
     if target is None or not start<=target<=end:exits.append(target)
   if exits:continue # this diagnostic supports one normal exit, not arbitrary CFG boundaries
   b=type('Bound',(),dict(BeginAddress=start,EndAddress=end,UnwindData=0,parts=[(start,end)]))
  directory=output/f'candidate-{len(plan):02d}';directory.mkdir(exist_ok=False)
  size=b.EndAddress-b.BeginAddress
  if not 0<size<=65536:raise ValueError('Selected function outside decode budget')
  (directory/'expected-code.bin').write_bytes(pe.get_data(b.BeginAddress,size))
  effective_mode=(mode if mode in ('iteration','bulk') else 'region') if loop_regions else mode if g['deep_eligible'] or mode=='boundary' else 'boundary'
  effective_events=max(32,min(events,64)) if effective_mode in ('boundary','region') else min(events,32768)
  effective_ms=min(ms,5000) if effective_mode in ('iteration','bulk','callee') else min(ms,250) if effective_mode in ('boundary','region') else min(ms,100)
  (directory/'request.txt').write_text(f'{b.BeginAddress:x} {size:x} {pe.FILE_HEADER.TimeDateStamp:x} {pe.OPTIONAL_HEADER.SizeOfImage:x} {effective_events} {effective_ms} {effective_mode} {max_span if g["exchange"] else 0} {int(outermost)} {int(stop_unsupported)} {boundary_samples if effective_mode in ("boundary","region") else 1} {iteration_skip} {iteration_samples} {int(defer_first_region)}\n')
  if effective_mode=='bulk':
   (directory/'view-budget.txt').write_text(str(view_budget_mib)+'\n')
   if pointer_target_plan:
    hints=json.loads(pointer_target_plan.read_text())
    if hints['module_sha256']!=digest or len(hints['records'])>128:raise ValueError('pointer-hint generation/budget mismatch')
    for record in hints['records']:
     code=bytes.fromhex(record['code'])
     if pe.get_data(record['rva'],len(code))!=code:raise ValueError('pointer-hint code changed')
    (directory/'pointer-load-sites.txt').write_text(str(len(hints['records']))+'\n'+' '.join(str(x['rva']) for x in hints['records'])+'\n')
    (directory/'pointer-load-plan.json').write_text(json.dumps(hints,indent=2))
  if entry_filter is not None:
   filt=json.loads(entry_filter.read_text())
   if b.BeginAddress<=filt['branch_rva']<b.EndAddress:
    if effective_mode not in ('iteration','bulk'):raise ValueError('entry filter requires iteration observer')
    (directory/'entry-filter.json').write_text(json.dumps(filt,indent=2))
    (directory/'entry-filter.txt').write_text(f"{filt['register_index']} {filt['mask']} {filt['value']}\n")
  decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
  pending=[b.BeginAddress];seen=set();checked=[];pure=[]
  if loop_regions:
   # Seed with the actual sampled region, then follow its calls before unrelated parent calls.
   bounds=dict(bounds);bounds[b.BeginAddress]=b
  while pending and len(seen)<128:
   address=pending.pop();address=owner.get(address,address)
   if address in seen or address not in bounds:continue
   seen.add(address);fn=bounds[address];blob=pe.get_data(fn.BeginAddress,fn.EndAddress-fn.BeginAddress)
   if len(blob)>65536:continue
   checked.append((fn.BeginAddress,blob))
   for ins in decoder.disasm(blob,fn.BeginAddress):
    if pure_instruction(ins):pure.append({'rva':ins.address,'size':ins.size,'bytes':ins.bytes.hex()})
    if ins.group(capstone.CS_GRP_CALL) and ins.operands and ins.operands[0].type==X86_OP_IMM:pending.append(ins.operands[0].imm)
  with (directory/'checked-functions.bin').open('wb') as sink:
   sink.write(struct.pack('<I',len(checked)))
   for address,blob in checked:sink.write(struct.pack('<II',address,len(blob)));sink.write(blob)
  if effective_mode in ('iteration','bulk','callee'):
   from cpu_capture_memory import build as memory_plan
   memory_plan(directory)
  (directory/'pure-rvas.bin').write_bytes(b''.join(struct.pack('<I',x['rva']) for x in pure))
  (directory/'pure-instructions.json').write_text(json.dumps(pure))
  plan.append(dict(directory=directory.name,entry_rva=b.BeginAddress,end_rva=b.EndAddress,detector_function_rva=f,unwind_chain=chains,logical_ranges=b.parts,effective_mode=effective_mode,effective_events=effective_events,effective_ms=effective_ms,requested_mode=mode,mode_reason='bounded research unavailable in current context' if effective_mode!=mode else None,**g))
 result=dict(image_sha256=digest,detector_sha256=hashlib.sha256((probe/'candidates.json').read_bytes()).hexdigest(),symbols_used=False,engine_source_used=False,selection='bounded research independent of replacement economics; rejected contexts are skipped',candidates=plan)
 (output/'selection.json').write_text(json.dumps(result,indent=2));return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('probe',type=Path);p.add_argument('output',type=Path);p.add_argument('--context',type=Path);p.add_argument('--boundary-samples',type=int,default=1);p.add_argument('--loop-regions',action='store_true');p.add_argument('--iteration-skip',type=int,default=0);p.add_argument('--iteration-samples',type=int,default=1);p.add_argument('--entry-filter',type=Path);p.add_argument('--view-budget-mib',type=int,default=128);p.add_argument('--pointer-target-plan',type=Path);p.add_argument('--defer-first-region',action='store_true');p.add_argument('--callee-from',type=Path);p.add_argument('--measurements',type=Path);p.add_argument('--mode',choices=['trace','boundary','training','consumer','iteration','callee','bulk'],default='trace');p.add_argument('--events',type=int,default=32768);p.add_argument('--ms',type=int,default=100);p.add_argument('--max-arg-span',type=int,default=0);p.add_argument('--outermost',action='store_true');p.add_argument('--stop-unsupported',action='store_true');a=p.parse_args();j=select(a.probe,a.output,a.mode,a.events,a.ms,a.max_arg_span,a.outermost,a.stop_unsupported,a.context,a.measurements,a.boundary_samples,a.loop_regions,a.iteration_skip,a.callee_from,a.iteration_samples,a.entry_filter,a.view_budget_mib,a.pointer_target_plan,a.defer_first_region);print('Selected',len(j['candidates']),'whole entry regions')
