import importlib.util
from pathlib import Path
import sys
import unittest
import numpy as np

root = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(root/'scripts'), str(root/'build/quality-worker')]
import cv2
from optimizer_quality_metrics import compare_striped, accepts
spec = importlib.util.spec_from_file_location('oracle', root/'scripts/optimizer-quality.py')
oracle = importlib.util.module_from_spec(spec); spec.loader.exec_module(oracle)
coordinates = np.arange(-5, 6)
weights = np.exp(-(coordinates**2)/(2*1.5**2)); weights /= weights.sum()
def gaussian(a):
    return cv2.sepFilter2D(a, cv2.CV_64F, weights, weights, borderType=cv2.BORDER_REFLECT_101)

class Metrics(unittest.TestCase):
    def test_independent_oracle(self):
        rng = np.random.default_rng(451)
        for h, w in [(11,11),(15,17),(128,131),(257,193),(1080,1920)]:
            a = rng.random((h,w,3)); b = np.clip(a+rng.normal(0,.025,a.shape),0,1)
            expected = oracle.compare(a,b)
            actual = compare_striped(a,b,filter_fn=gaussian)
            for key in expected:
                if key.endswith('_pass'): self.assertEqual(actual[key], expected[key])
                else: self.assertLessEqual(abs(actual[key]-expected[key]), 1e-7, (h,w,key))
    def test_profiles_and_small_defect(self):
        m=dict(ssim_gaussian_luma=.96,mean_linear_rgb_error=.02,p99_tile_linear_rgb_error=.08,worst_tile_linear_rgb_error=.2)
        self.assertFalse(accepts(m)); self.assertTrue(accepts(m,'aggressive'))
        m['worst_tile_linear_rgb_error']=.26; self.assertFalse(accepts(m,'aggressive'))
    def test_nonfinite(self):
        a=np.zeros((17,17,3)); b=a.copy(); b[8,8,0]=np.nan
        with self.assertRaises(ValueError): compare_striped(a,b,filter_fn=gaussian)

if __name__ == '__main__': unittest.main()
