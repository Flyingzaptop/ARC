import copy,json,sys,tempfile,unittest,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'scripts'))
from analyze_cpu_batch import analyze,expression
from cpu_batch_plan import plan
class PacketTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
  with zipfile.ZipFile(ROOT/'docs/evidence/cpu-batch-20260927/raw.zip') as z:
   for name in z.namelist():
    if name.startswith(('batch/','slice/','consumers/','watch/')):z.extract(name,self.root)
 def tearDown(self):self.temp.cleanup()
 def mutate(self,fn):
  p=self.root/'batch/groups.json';data=json.loads(p.read_text());fn(data);p.write_text(json.dumps(data))
 def test_heldout_packets(self):
  r=analyze(self.root);self.assertEqual(r['status'],'validated_local_packets');self.assertEqual(sum(x['elements'] for x in r['groups']),768);self.assertFalse(r['replacement_allowed']);self.assertIsNone(r['removable_cpu_ms']);self.assertEqual(r['groups'][-1]['role'],'holdout')
 def test_wrong_bound(self):
  self.mutate(lambda g:g[0].update(end=g[0]['end']+1));self.assertEqual(analyze(self.root)['status'],'invalid')
 def test_wrong_argument_index(self):
  self.mutate(lambda g:g[0]['rows'][0].update(argument_index=999));self.assertEqual(analyze(self.root)['status'],'invalid')
 def test_output_corruption(self):
  self.mutate(lambda g:g[0]['rows'][0].update(output_after='00'*12));self.assertEqual(analyze(self.root)['status'],'invalid')
 def test_stale_inputs_not_equivalent(self):
  self.mutate(lambda g:g[0]['rows'][0].update(input_ready=g[0]['rows'][0]['input_before']));self.assertEqual(analyze(self.root)['status'],'invalid')
 def test_restoration_required(self):
  p=self.root/'batch/done.json';v=json.loads(p.read_text());v['restored']=0;p.write_text(json.dumps(v));self.assertEqual(analyze(self.root)['status'],'invalid')
 def test_independent_replanning(self):
  r=plan(self.root/'watch',self.root/'slice',self.root/'newplan');old=json.loads((self.root/'batch/plan.json').read_text());self.assertEqual(r,old)
 def test_invalid_budget(self):
  with self.assertRaisesRegex(ValueError,'budget'):plan(self.root/'watch',self.root/'slice',self.root/'newplan',groups=4)
 def test_unknown_expression_rejected(self):
  with self.assertRaises(ValueError):expression({'kind':'live_register'},lambda a:b'\0'*4)
 def test_read_witness_not_ownership(self):
  r=analyze(self.root)['consumers'];self.assertTrue(any(x['same_thread_and_function'] for x in r['witnesses']));self.assertFalse(r['complete_thread_coverage']);self.assertFalse(r['absence_of_earlier_consumers_proven'])
if __name__=='__main__':unittest.main()
