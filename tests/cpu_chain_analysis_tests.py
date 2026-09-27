import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_chain_analysis import boundaries
class CostScopeTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);self.d=self.root/'candidate-00';self.d.mkdir()
  (self.root/'selection.json').write_text(json.dumps({'candidates':[{'directory':'candidate-00','ids':['test'],'entry_rva':4096}]}))
  (self.d/'expected-code.bin').write_bytes(bytes.fromhex('4983c7104d39d775f7'))
  self.meta={'streaming':False,'reason':0,'trap_cleanup_pending':False,'boundary_mode':True,'region_mode':True,'completed_calls':2,'qpc_frequency':1000}
  self.events=[]
  for start in [100,200]:
   a={'kind':1,'tid':7,'registers':[0]*16};a['registers'][15]=start
   b={'kind':2,'tid':7,'registers':[0]*16};b['registers'][15]=start+32;self.events.extend([a,b])
  (self.d/'boundary-costs.json').write_text(json.dumps([{'tid':7,'begin_qpc':10,'end_qpc':11,'handler_ticks_inside':0}]*2));self.save()
 def save(self):
  (self.d/'capture.json').write_text(json.dumps(self.meta));(self.d/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in self.events))
 def tearDown(self):self.tmp.cleanup()
 def test_first_partial_is_excluded_and_cost_not_promoted(self):
  r=boundaries(self.root)[0];self.assertEqual(r['wall_ms']['n'],1);self.assertEqual(r['discarded_first_partial_per_thread'],1);self.assertEqual(r['observed_elements']['median'],2);self.assertIsNone(r['removable_cpu_ms']);self.assertIsNone(r['critical_path_frame_ms']);self.assertIsNone(r['calls_per_frame'])
 def test_bad_pair_rejected(self):
  self.events[-1]['tid']=8;self.save()
  with self.assertRaises(ValueError):boundaries(self.root)
 def test_pending_trap_rejected(self):
  self.meta['trap_cleanup_pending']=True;self.save()
  with self.assertRaises(ValueError):boundaries(self.root)
 def test_unknown_induction_stays_unknown(self):
  (self.d/'expected-code.bin').write_bytes(bytes.fromhex('9090'));self.assertIsNone(boundaries(self.root)[0]['observed_elements'])
if __name__=='__main__':unittest.main()
