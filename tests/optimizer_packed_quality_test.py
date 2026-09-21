import os
for key in ('OPENBLAS_NUM_THREADS','OMP_NUM_THREADS','MKL_NUM_THREADS'):
    os.environ[key]='1'
import sys,runpy,unittest
from pathlib import Path
import numpy as np
root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'scripts'),str(root/'build/quality-worker')]
import cv2
live=runpy.run_path(str(root/'scripts/optimizer-live-quality.py'))
oracle=runpy.run_path(str(root/'scripts/optimizer-quality.py'))
from optimizer_quality_metrics import compare_striped

class PackedQuality(unittest.TestCase):
    def test_unscaled_bilinear_and_linear_lookup(self):
        rng=np.random.default_rng(6451)
        for divisor in (255,1023):
            raw=rng.integers(0,divisor+1,(37,61,3),dtype=np.uint16)
            x,y=np.meshgrid(np.arange(61,dtype=np.float32)+.237,np.arange(37,dtype=np.float32)-.719)
            full=cv2.remap(raw.astype(np.float64)/divisor,x,y,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
            packed=live['remap_image'](live['PackedImage'](raw,divisor),x,y,packed=True)
            self.assertLess(np.max(np.abs(full-packed[:])),1e-12)
            self.assertLess(np.max(np.abs(cv2.pow(full,2.2)-packed.linear_region(slice(None)))),1e-12)
            other=live['PackedImage'](np.roll(raw,1,axis=1),divisor)
            expected=oracle['compare'](full,other[:])
            actual=compare_striped(packed,other,filter_fn=live['fast_gaussian'])
            for key,value in expected.items():
                if isinstance(value,bool):self.assertEqual(value,actual[key])
                else:self.assertLessEqual(abs(value-actual[key]),1e-7,key)

    def test_motion_decision_and_full_resolution(self):
        rng=np.random.default_rng(451)
        for h,w in ((128,192),(1080,1920)):
            before=rng.integers(0,256,(h,w,3),dtype=np.uint8)
            after=np.roll(before,2,axis=1);candidate=np.roll(before,1,axis=1).copy()
            candidate[h//3:h//3+8,w//3:w//3+8]//=2
            full=live['assess'](*(p.astype(np.float64)/255 for p in (before,candidate,after)),.5,'aggressive')
            packed=live['assess'](*(live['PackedImage'](p) for p in (before,candidate,after)),.5,'aggressive')
            for section in ('quality','reference_check'):
                for key,value in full[section].items():
                    if isinstance(value,bool):self.assertEqual(value,packed[section][key])
                    else:self.assertLessEqual(abs(value-packed[section][key]),1e-7,(section,key))
            self.assertEqual(full['matched_reference'],packed['matched_reference'])
            self.assertEqual(full['accepted_quality'],packed['accepted_quality'])

if __name__=='__main__':unittest.main()
