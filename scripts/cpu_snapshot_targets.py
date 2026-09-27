"""Pointer-load hints derived only from address dependencies in a captured model."""
import argparse,json
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_REG,X86_OP_MEM
from cpu_composite_ir import analyze_paths
from cpu_composite_gather import _dependencies

def build(folder):
 paths,groups=analyze_paths(folder)
 if not paths or paths[0]['status']!='closed_observed_path':raise ValueError('closed observed path required')
 ir=paths[0]['typed_ir'];meta=json.loads((folder/'capture.json').read_text());dependencies=set()
 for access in ir['memory_accesses']:dependencies.update(_dependencies(ir['nodes'],access['address_source']))
 leaves=[n for n in ir['nodes'] if n['id'] in dependencies and n['op']=='memory_input' and len(bytes.fromhex(n['bytes']))==8]
 decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True;records={}
 for node in leaves:
  for event in groups[0]:
   ins=next(decoder.disasm(bytes.fromhex(event['code']),event['rip']),None)
   if ins and ins.mnemonic=='mov' and len(ins.operands)==2 and ins.operands[0].type==X86_OP_REG and ins.operands[1].type==X86_OP_MEM and ins.operands[0].size==8:
    if any(m['address']==node['address'] and m['size']==8 and m['ok'] and m['bytes']==node['bytes'] for m in event['memory']):
     rva=event['rip']-meta['main_base'];records[rva]={'rva':rva,'code':ins.bytes.hex(),'role':'observed memory pointer used by subsequent address expressions'};break
 return {'source':str(folder),'module_sha256':json.loads((folder.parent/'selection.json').read_text())['image_sha256'],'records':list(records.values()),'scope':'bounded snapshot coverage hints; not pointer type or live ownership proof'}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);a=p.parse_args();r=build(a.capture);a.output.write_text(json.dumps(r,indent=2));print(len(r['records']))
