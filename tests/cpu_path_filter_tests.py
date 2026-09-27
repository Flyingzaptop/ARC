import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_path_filter import build
class PathFilterTests(unittest.TestCase):
 def make(self,root,name,value,write=False):
  d=root/name;d.mkdir();(d/'capture.json').write_text(json.dumps({'streaming':False,'main_base':0x100000}))
  regs=[0]*16;regs[14]=value
  rows=[];pc=0x101000
  if write:rows.append({'kind':1,'rip':pc,'code':'41b600','registers':regs});pc+=3
  rows.append({'kind':0 if write else 1,'rip':pc,'code':'4180fe00','registers':regs})
  rows.append({'kind':0,'rip':pc+4,'code':'7502','registers':regs})
  rows.append({'kind':2,'rip':pc+6+(2 if value else 0),'code':'90','registers':regs})
  (d/'events.jsonl').write_text('\n'.join(json.dumps(x) for x in rows));return d
 def test_stable_discriminator_not_admission(self):
  with tempfile.TemporaryDirectory() as t:
   r=Path(t);p=self.make(r,'positive',0);n=self.make(r,'negative',1);x=build(p,n)
   self.assertEqual((x['register'],x['mask'],x['value']),('r14',255,0));self.assertIn('not replacement guard',x['scope'])
 def test_overwritten_entry_value_is_not_filter(self):
  with tempfile.TemporaryDirectory() as t:
   r=Path(t);p=self.make(r,'positive',0,True);n=self.make(r,'negative',1,True)
   with self.assertRaisesRegex(ValueError,'stable'):build(p,n)
 def test_changed_code_rejected(self):
  with tempfile.TemporaryDirectory() as t:
   r=Path(t);p=self.make(r,'positive',0);n=self.make(r,'negative',1);f=n/'events.jsonl';f.write_text(f.read_text().replace('4180fe00','4180fe01'))
   with self.assertRaisesRegex(ValueError,'generation'):build(p,n)
if __name__=='__main__':unittest.main()
