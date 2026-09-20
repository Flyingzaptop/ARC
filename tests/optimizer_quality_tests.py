"""Independent metric sanity cases, including partial tiles and invalid data."""
import runpy
from pathlib import Path
import numpy as np

compare = runpy.run_path(str(Path(__file__).resolve().parents[1] / "scripts/optimizer-quality.py"))["compare"]
image = np.full((37, 61, 3), .4)
same = compare(image, image)
assert abs(same["ssim_gaussian_luma"] - 1) < 1e-12
assert same["mean_linear_rgb_error"] == 0 and same["moderate_pass"]
changed = image.copy()
changed[-1, -1] = 1
edge = compare(image, changed)
assert edge["worst_tile_linear_rgb_error"] > edge["mean_linear_rgb_error"] > 0
assert np.isfinite(edge["ssim_gaussian_luma"])
stripes = np.zeros((64, 64, 3))
stripes[:, ::2] = 1
blurred = np.full_like(stripes, .5)
assert not compare(stripes, blurred)["moderate_pass"]
invalid = image.copy()
invalid[0, 0, 0] = np.nan
try:
    compare(image, invalid)
    raise AssertionError("NaN image was accepted")
except ValueError:
    pass
print("Independent SSIM/error, partial tiles, detail loss and NaN rejection passed")
