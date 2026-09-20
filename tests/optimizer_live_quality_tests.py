import runpy
from pathlib import Path
import numpy as np

module = runpy.run_path(str(Path(__file__).resolve().parents[1]/"scripts/optimizer-live-quality.py"))
assess = module["assess"]
image = np.full((64, 96, 3), .3)
image[16:48, 24:72] = .7
same = assess(image, image, image)
assert same["matched_reference"] and same["accepted_quality"]
damaged = image.copy(); damaged[20:40, 30:60] = 0
bad = assess(image, damaged, image)
assert bad["matched_reference"] and not bad["accepted_quality"]
assert not assess(image, image, np.ones_like(image))["matched_reference"]
assert not assess(np.zeros_like(image),np.zeros_like(image),np.zeros_like(image))["matched_reference"]
for value in (float("nan"),float("inf"),-.1,1.1):
    invalid=image.copy();invalid[0,0,0]=value
    try:
        assess(image,invalid,image)
    except ValueError:
        pass
    else:
        raise AssertionError("Invalid pixels must refuse the trial")
print("Live critic accepts identical references, rejects damage and refuses scene cuts")
samples=[]
for index,offset in enumerate((0,.5,1)):
    words=(np.arange(8,dtype=np.float32).reshape(2,4)+1+offset).view(np.uint32).tolist()
    samples.append({"pipeline":1,"submission":index+1,"keys":[[0,0,0],[0,0,16]],"words":words,"valid":[15,15]})
timing=module["state_fraction"](samples)
assert timing and timing["fraction"]==.5 and timing["components"]==8
samples[1]["pipeline"]=2
assert module["state_fraction"](samples) is None
samples[1]["pipeline"]=1
samples[1]["words"]=(np.arange(8,dtype=np.float32).reshape(2,4)+1+np.array([[.2,.3,.4,.5],[.6,.7,.8,.9]],dtype=np.float32)).view(np.uint32).tolist()
assert module["state_fraction"](samples) is None
assert module["state_fraction"]([]) is None
print("Original-state timing accepts consistent simulation motion and rejects mismatched state")

# Acceleration may change summation order, not the quality definition or limits.
rng = np.random.default_rng(8172)
for shape in ((11,11),(17,23),(64,65)):
    values = rng.random(shape)
    np.testing.assert_allclose(module["fast_gaussian"](values),module["metrics"]["gaussian"](values),rtol=0,atol=1e-14)
    a = rng.random((*shape,3));b = np.clip(a+rng.normal(0,.003,a.shape),0,1)
    slow = module["metrics"]["compare"](a,b)
    fast = module["metrics"]["compare"](a,b,filter_fn=module["fast_gaussian"])
    assert slow["moderate_pass"] == fast["moderate_pass"]
    for key in slow:
        if key != "moderate_pass": assert abs(slow[key]-fast[key]) < 1e-12
    error=np.abs(a**2.2-b**2.2)
    tiles=[float(error[y:y+8,x:x+8].mean()) for y in range(0,shape[0],8) for x in range(0,shape[1],8)]
    assert abs(fast["p99_tile_linear_rgb_error"]-np.quantile(tiles,.99))<1e-14
    assert abs(fast["worst_tile_linear_rgb_error"]-max(tiles))<1e-14
print("Accelerated full-resolution metrics agree with independent Gaussian and partial-tile oracles")
