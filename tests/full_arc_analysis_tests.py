import csv,importlib.util,json,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location("analysis",Path(__file__).resolve().parents[1]/"scripts/analyze-full-arc-comparison.py")
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
class FrameJoinTests(unittest.TestCase):
 def test_gpu_joins_source_not_readout_frame_and_preserves_outlier(self):
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp);(d/'manifest.json').write_text(json.dumps(dict(warmup_seconds=1,measurement_seconds=3)))
   with (d/'cpu.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['elapsed_ms','frame','scene','event','ms'])
    w.writerows([[1100,10,18,'CPU processing through submission ms',7],[1100,10,18,'Controlled Present interval',8],[1200,11,18,'CPU processing through submission ms',100],[1200,11,18,'Controlled Present interval',100],[1400,10,18,'Retired GPU frame span ms',3],[1500,11,18,'Retired GPU frame span ms',4]])
   rows,_=a.wicked(d)
   self.assertEqual([r['gpu_ms'] for r in rows],[3,4]);self.assertEqual(a.stat([r['cpu_ms'] for r in rows])['max'],100)
 def test_missing_gpu_tail_is_not_fabricated(self):
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp);(d/'manifest.json').write_text('{"warmup_seconds":0,"measurement_seconds":1}');(d/'cpu.csv').write_text('elapsed_ms,frame,scene,event,ms\n10,3,18,CPU processing through submission ms,5\n')
   rows,_=a.wicked(d);self.assertIsNone(rows[0]['gpu_ms'])
if __name__=='__main__':unittest.main()
