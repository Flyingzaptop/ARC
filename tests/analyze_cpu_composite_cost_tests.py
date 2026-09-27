import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_cpu_composite_cost import GPU_COMPONENTS, analyze, write_analysis


class CompositeCostTests(unittest.TestCase):
    def fixture(self,folder):
        cpu={'packet_count':2,'after_snapshot_equal':True,'warm_comparison_admitted':True,
             'warm_samples_ms':[1.0,1.0,5.0]}
        nested={'returncode':0,'stdout':json.dumps(cpu)}
        (folder/'append-original.json').write_text(json.dumps({'returncode':0,'stdout':json.dumps(nested)}))
        samples=[]
        for value in (2.0,3.0,4.0):
            samples.append({'returncode':0,'stderr':'','stdout':
                            f'rows=2 row_bytes=8 gather_ms={value} verification_ms=0.5 '
                            f'full_prep_ms={value+0.5} first_two_exact=1 bounded_snapshot_reads=1'})
        gather={'samples':samples,'original_input_sha256':'same','rerun_input_sha256':'same',
                'executable_sha256':'executable'}
        (folder/'append-gather.json').write_text(json.dumps(gather))
        names=['iteration','count',*GPU_COMPONENTS,'validated','mismatch_words']
        with (folder/'fused-tail.csv').open('w',newline='') as output:
            writer=csv.DictWriter(output,fieldnames=names);writer.writeheader()
            for iteration in range(-1,20):
                row={key:0.0 for key in GPU_COMPONENTS};row['full_ms']=1.0
                row.update(iteration=iteration,count=2,validated=int(iteration in (-1,19)),mismatch_words=0)
                writer.writerow(row)
        (folder/'results.json').write_text(json.dumps({'count':2,'external_native_gather_ms':1.5,
                                                       'files_sha256':{}}))

    def test_raw_reruns_recompute_mean_median_and_preserve_prior(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);self.fixture(folder)
            result=write_analysis(folder,'append')
            self.assertEqual(result['gather_samples_ms'],[2.0,3.0,4.0])
            self.assertEqual(result['gather_median_ms'],3.0)
            self.assertEqual(result['composed_nonoverlapped_median_ms'],4.0)
            self.assertAlmostEqual(result['regression_vs_cpu_warm_mean_ms'],4.0-7/3)
            self.assertEqual(result['prior_single_gather_ms'],1.5)
            self.assertEqual(result,json.loads((folder/'cost-analysis.json').read_text()))
            self.assertEqual(result,analyze(folder,'append'))
            summary=json.loads((folder/'results.json').read_text())
            self.assertEqual(summary['external_native_gather_ms'],3.0)
            self.assertEqual(summary['prior_single_gather_ms'],1.5)

    def test_identity_or_final_gpu_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);self.fixture(folder)
            gather=json.loads((folder/'append-gather.json').read_text())
            gather['rerun_input_sha256']='different'
            (folder/'append-gather.json').write_text(json.dumps(gather))
            with self.assertRaisesRegex(ValueError,'identity'):analyze(folder,'append')
            gather['rerun_input_sha256']='same'
            (folder/'append-gather.json').write_text(json.dumps(gather))
            path=folder/'fused-tail.csv'
            with path.open(newline='') as source:rows=list(csv.DictReader(source))
            rows[-1]['mismatch_words']='1'
            with path.open('w',newline='') as output:
                writer=csv.DictWriter(output,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
            with self.assertRaisesRegex(ValueError,'bytes differ'):analyze(folder,'append')


if __name__=='__main__':unittest.main()
