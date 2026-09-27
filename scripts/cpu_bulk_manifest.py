"""Join automatic loop, append, and bounded VM snapshot evidence for isolated replay."""
import argparse,hashlib,json,struct
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_IMM,X86_OP_REG,X86_OP_MEM
from cpu_append_contract import derive_append_contract,compose_append_trace
from cpu_producer_plan import GPRS
from cpu_cfg_constants import canonical

def build(folder,append_capture,image):
 views=json.loads((folder/'views.json').read_text());selection=json.loads((folder.parent/'selection.json').read_text())
 if not views['completed'] or views['missed_regions'] or not all(x['before_ok'] and x['after_ok'] for x in views['regions']):raise ValueError('snapshot collection incomplete')
 if hashlib.sha256(image.read_bytes()).hexdigest()!=selection['image_sha256']:raise ValueError('image generation mismatch')
 def read(address,size,side='before'):
  result=bytearray()
  while size:
   region=next((r for r in views['regions'] if r['base']<=address<r['base']+r['size']),None)
   if region is None:raise ValueError('snapshot range missing')
   n=min(size,region['base']+region['size']-address)
   with (folder/region[side+'_file']).open('rb') as f:f.seek(address-region['base']);data=f.read(n)
   if len(data)!=n:raise ValueError('snapshot file truncated')
   result.extend(data);address+=n;size-=n
  return bytes(result)
 append=derive_append_contract(append_capture);composition=compose_append_trace(folder,append)
 if not composition.get('all_record_bytes_verified'):raise ValueError('prefix output bytes unverified')
 entry=(folder/'entry-context.bin').read_bytes();exit=(folder/'exit-context.bin').read_bytes();regs=struct.unpack_from('<16Q',entry,120);out=struct.unpack_from('<16Q',exit,120)
 d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True;ins=list(d.disasm((folder/'expected-code.bin').read_bytes(),views['loop_entry_rva']));add,compare,branch=ins[-3:]
 if not(add.mnemonic=='add' and add.operands[0].type==X86_OP_REG and add.operands[1].type==X86_OP_IMM and compare.mnemonic=='cmp' and compare.operands[0].type==X86_OP_REG and compare.operands[1].type==X86_OP_MEM and branch.mnemonic=='jne' and branch.operands[0].imm==views['loop_entry_rva']):raise ValueError('unsupported counted tail')
 name=canonical(add.reg_name(add.operands[0].reg));idx=GPRS.index(name);step=add.operands[1].imm
 if canonical(compare.reg_name(compare.operands[0].reg))!=name or step<=0 or out[idx]<=regs[idx] or (out[idx]-regs[idx])%step:raise ValueError('invalid induction bounds')
 m=compare.operands[1].mem
 if m.index or m.segment or compare.operands[1].size!=8:raise ValueError('unsupported loop bound address')
 bound_address=regs[GPRS.index(canonical(compare.reg_name(m.base)))]+m.disp;bound=struct.unpack('<Q',read(bound_address,8))[0]
 if bound!=out[idx] or struct.unpack_from('<Q',exit,248)[0]!=views['module_base']+views['loop_end_rva']:raise ValueError('natural loop end mismatch')
 count=(out[idx]-regs[idx])//step;header=composition['container_address_observed'];cursor_address=header+append['container']['cursor_offset'];cursor=struct.unpack('<Q',read(cursor_address,8))[0];final=struct.unpack('<Q',read(cursor_address,8,'after'))[0];capacity=struct.unpack('<Q',read(header+append['container']['capacity_offset'],8))[0];stride=append['record_stride']
 if cursor!=composition['initial_cursor_observed'] or final<cursor or final>capacity or (final-cursor)%stride:raise ValueError('cursor composition mismatch')
 memory=json.loads((folder/'memory-plan.json').read_text());gs=[op for row in memory['records'] for op in row['operands'] if op['base']==17]
 if not gs or len({(x['disp'],x['size']) for x in gs})!=1 or gs[0]['size']!=8:raise ValueError('unsupported GS input set')
 tls=struct.unpack('<Q',read(views['teb']+gs[0]['disp'],8))[0]
 manifest=dict(views,module_image=str(image.resolve()),entry_context_hex=entry.hex(),exit_context_hex=exit.hex(),teb_tls_pointer_value=tls,capture=str(folder.resolve()),append_contract=str((folder/'append-contract.json').resolve()),packet_count=count,initial_cursor=cursor,expected_final_cursor=final,capacity_end=capacity,cursor_address=cursor_address,record_stride=stride,induction_register=name,induction_step=step)
 for r in manifest['regions']:r['before_file']=str((folder/r['before_file']).resolve());r['after_file']=str((folder/r['after_file']).resolve())
 (folder/'append-contract.json').write_text(json.dumps(append,indent=2));(folder/'composition.json').write_text(json.dumps(composition,indent=2));(folder/'expected-records.bin').write_bytes(read(cursor,final-cursor,'after'));(folder/'native-manifest.json').write_text(json.dumps(manifest,indent=2));return manifest
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('append_capture',type=Path);p.add_argument('image',type=Path);a=p.parse_args();r=build(a.capture,a.append_capture,a.image);print(r['packet_count'],(r['expected_final_cursor']-r['initial_cursor'])//r['record_stride'])
