"""Ensure per-packet waits are summed, but overlapped gaps are not."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AsyncAttribution(unittest.TestCase):
    def test_consumer_boundary_and_incomplete_packet_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            with (root/'Q1.csv').open('w',newline='') as stream:
                w=csv.writer(stream)
                w.writerow(['elapsed_ms','frame','scene','event','ms'])
                for time,frame,event,value in [
                    (6010,20,'Chain in flight packets',2),
                    (6012,20,'Chain consumer wait ms',.5),
                    (6013,20,'Chain consumer wait ms',.2),
                    (6014,20,'Chain independent gap ms',4),
                    (6020,20,'SubmitCommandLists',1),
                    (6030,21,'Chain in flight packets',2),
                    (6032,21,'Chain consumer wait ms',.8), # missing second packet
                    (6040,21,'SubmitCommandLists',1),
                ]:w.writerow([time,frame,18,event,value])
            subprocess.run([sys.executable,str(Path(__file__).with_name('analyze_async.py')),str(root)],check=True,capture_output=True)
            result=json.loads((root/'comparison.json').read_text())['runs']['Q1']
            self.assertEqual(result['cadence']['mean'],20)
            metrics=result['chain_per_frame']
            self.assertAlmostEqual(metrics['Chain consumer wait ms']['mean'],.7)
            self.assertEqual(metrics['independent_CPU_interval_ms']['count'],1)
            self.assertAlmostEqual(metrics['independent_CPU_interval_ms']['mean'],1.5)


if __name__=='__main__':unittest.main()
