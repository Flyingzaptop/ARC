import runpy
from pathlib import Path
import numpy as np

assess = runpy.run_path(str(Path(__file__).resolve().parents[1]/"scripts/optimizer-live-quality.py"))["assess"]
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
