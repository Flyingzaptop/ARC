"""Select whole entry regions exclusively from detector output; no symbols/game offsets."""
import argparse,hashlib,json
from pathlib import Path
import pefile,capstone,struct
from capstone.x86 import X86_OP_MEM,X86_OP_IMM
PURE={"mov","movzx","movsx","movsxd","lea","add","sub","and","or","xor","shl","shr","sar","sal","inc","dec","cmp","test","neg","not","pxor"}
def pure_instruction(i):return i.mnemonic in PURE and (i.mnemonic=="lea" or not any(x.type==X86_OP_MEM for x in i.operands))

def select(probe,output,mode="trace",events=32768,ms=100,max_span=0,outermost=False,stop_unsupported=False):
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
 for c in j['candidates']:
  f=c['function_rva'];g=groups.setdefault(f,{'samples':0,'ids':[],'exchange':False});g['samples']=max(g['samples'],c['samples']);g['ids'].append(c['id']);g['exchange']|=bool(c.get('record_exchanges'))
 output.mkdir(parents=True,exist_ok=True);plan=[]
 for f,g in sorted(groups.items(),key=lambda x:(not x[1]['exchange'],-x[1]['samples'],x[0])):
  logical=owner.get(f,f)
  if logical not in bounds:continue # unknown split layout stays unselected, never guessed
  b=bounds[logical];chains=[]
  for _ in range(8):
   raw=pe.get_data(b.UnwindData,4)
   if not raw or not (raw[0]>>3)&4:break
   offset=b.UnwindData+4+((raw[2]+1)//2)*4
   begin,end,unwind=struct.unpack('<III',pe.get_data(offset,12));chains.append(f);b=type('Bound',(),dict(BeginAddress=begin,EndAddress=end,UnwindData=unwind))
  directory=output/f'candidate-{len(plan):02d}';directory.mkdir(exist_ok=False)
  size=b.EndAddress-b.BeginAddress
  if not 0<size<=65536:raise ValueError('Selected function outside decode budget')
  (directory/'expected-code.bin').write_bytes(pe.get_data(b.BeginAddress,size))
  (directory/'request.txt').write_text(f'{b.BeginAddress:x} {size:x} {pe.FILE_HEADER.TimeDateStamp:x} {pe.OPTIONAL_HEADER.SizeOfImage:x} {events} {ms} {mode} {max_span if g["exchange"] else 0} {int(outermost)} {int(stop_unsupported)}\n')
  decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
  pending=[b.BeginAddress];seen=set();checked=[];pure=[]
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
  (directory/'pure-rvas.bin').write_bytes(b''.join(struct.pack('<I',x['rva']) for x in pure))
  (directory/'pure-instructions.json').write_text(json.dumps(pure))
  plan.append(dict(directory=directory.name,entry_rva=b.BeginAddress,end_rva=b.EndAddress,detector_function_rva=f,unwind_chain=chains,logical_ranges=b.parts,**g))
 result=dict(image_sha256=digest,detector_sha256=hashlib.sha256((probe/'candidates.json').read_bytes()).hexdigest(),symbols_used=False,engine_source_used=False,selection='paired-exchange hint then sampled activity; not a proven critical-path rank',candidates=plan)
 (output/'selection.json').write_text(json.dumps(result,indent=2));return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('probe',type=Path);p.add_argument('output',type=Path);p.add_argument('--mode',choices=['trace','boundary','training','consumer'],default='trace');p.add_argument('--events',type=int,default=32768);p.add_argument('--ms',type=int,default=100);p.add_argument('--max-arg-span',type=int,default=0);p.add_argument('--outermost',action='store_true');p.add_argument('--stop-unsupported',action='store_true');a=p.parse_args();j=select(a.probe,a.output,a.mode,a.events,a.ms,a.max_arg_span,a.outermost,a.stop_unsupported);print('Selected',len(j['candidates']),'whole entry regions')
