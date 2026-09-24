"""Exact byte coverage from saved memory events with bounded page bitmaps."""
import argparse,csv,json
from pathlib import Path
class Coverage:
 def __init__(self):self.pages={};self.overflow=False
 def add(self,lo,size):
  while size:
   page=lo>>12;offset=lo&4095;n=min(size,4096-offset)
   if page not in self.pages:
    if len(self.pages)>=8192:self.overflow=True;return
    self.pages[page]=bytearray(512)
   data=self.pages[page];first=offset//8;last=(offset+n+7)//8;old=int.from_bytes(data[first:last],'little');mask=((1<<n)-1)<<(offset%8);new=old|mask
   if old!=new:data[first:last]=new.to_bytes(last-first,'little')
   lo+=n;size-=n
 def ranges(self):
  result=[];start=None;end=None
  for page,data in sorted(self.pages.items()):
   for index,value in enumerate(data):
    for bit in range(8):
     if not(value&(1<<bit)):continue
     address=(page<<12)+index*8+bit
     if start is None:start=address;end=address+1
     elif address==end:end+=1
     else:result.append([start,end]);start=address;end=address+1
  if start is not None:result.append([start,end])
  return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();r,w=Coverage(),Coverage()
 with (a.directory/'memory.csv').open() as f:
  for row in csv.DictReader(f):
   if row['in_call']!='True':continue
   if row['read']=='True':r.add(int(row['address']),int(row['size']))
   if row['write']=='True':w.add(int(row['address']),int(row['size']))
 j=json.loads((a.directory/'contract.json').read_text());m=json.loads((a.directory/'capture.json').read_text());j['observed_input_ranges']=r.ranges();j['observed_output_ranges']=w.ranges();j['non_stack_output_candidates']=[x for x in j['observed_output_ranges'] if not m['stack_low']<=x[0]<m['stack_high']]
 evidence=j['properties']['memory_footprint']['evidence'];complete=bool(j['exit']) and not m.get('boundary_mode') and not r.overflow and not w.overflow and not evidence['unresolved_effects'] and not evidence['flow_discontinuities'] and not m.get('semantic_stop')
 j['observed_footprint_complete']=complete;j['properties']['memory_footprint']['status']='observed' if complete else 'unknown';j['coverage_refinement']={'read_pages':len(r.pages),'write_pages':len(w.pages),'bytes_of_bitmap_storage':512*(len(r.pages)+len(w.pages)),'capacity_pages_per_direction':8192,'overflow':r.overflow or w.overflow}
 (a.directory/'contract.json').write_text(json.dumps(j,indent=2));print('Observed footprint complete:',complete,'bitmap bytes',j['coverage_refinement']['bytes_of_bitmap_storage'])
