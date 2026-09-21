import importlib.util
import json
import mmap
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import numpy as np

root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'scripts'),str(root/'build/quality-worker')]
spec=importlib.util.spec_from_file_location('live',root/'scripts/optimizer-live-quality.py')
live=importlib.util.module_from_spec(spec);spec.loader.exec_module(live)

class Worker(unittest.TestCase):
    def test_persistent_shared_images_and_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp);paths=[];shared=[];mappings=[]
            rng=np.random.default_rng(43);pixels=rng.integers(0,256,(128,192,4),dtype=np.uint8)
            for i in range(5):
                name=f'Local\\ARC-quality-test-{os.getpid()}-{i}'
                mapping=mmap.mmap(-1,pixels.nbytes,tagname=name);mapping.write(pixels.tobytes());mappings.append(mapping)
                path=directory/f'{i}.json';path.write_text(json.dumps(dict(readback_complete=True,color_space_known=True,gpu_features_only=False,present_hresult=0,color_space=0,width=192,height=128,pixel_file=f'{i}.pixels',dxgi_format=28,capture_qpc=i+1)))
                paths.append(str(path));shared.append(dict(name=name,bytes=pixels.nbytes))
            process=subprocess.Popen([sys.executable,str(root/'scripts/optimizer-live-quality.py'),'--serve'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                pids=[]
                for request_id in (1,2):
                    context=dict(surface_revision=5,captured_frame=23,candidate_epochs=[10,12])
                    request=dict(schema=1,request_id=request_id,context=context,profile='balanced',paths=paths,shared=shared)
                    process.stdin.write(json.dumps(request)+'\n');process.stdin.flush()
                    result=json.loads(process.stdout.readline());self.assertTrue(result['accepted_quality'],result)
                    self.assertEqual(result['context'],context);self.assertEqual(result['request_id'],request_id);pids.append(result['worker_pid'])
                self.assertEqual(pids[0],pids[1])
                process.stdin.close();self.assertEqual(process.wait(timeout=15),0)
            finally:
                if process.poll() is None: process.kill();process.wait()
                process.stdout.close();process.stderr.close()
                for mapping in mappings: mapping.close()
    def test_flicker_detected_using_original_motion(self):
        rng=np.random.default_rng(4);reference=rng.random((128,192,3))
        first=np.full_like(reference,.1);second=-first
        result=live.temporal(reference,reference,first,second)
        self.assertTrue(result['matched_reference']);self.assertGreater(result['p99_tile_error'],.06)

if __name__=='__main__': unittest.main()
