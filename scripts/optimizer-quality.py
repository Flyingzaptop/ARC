"""Independent SDR image checks. Does not read optimizer decisions or timings.

SSIM: encoded Rec.709-weighted luma, 11x11 Gaussian (sigma 1.5), population
covariance, C1=.01^2/C2=.03^2, excluding the five-pixel filter border.
Error: linear RGB after the benchmark's documented gamma-2.2 encoding.
Tiles: 8x8, with correctly weighted partial edge tiles. Limits are fixed by the
approved moderate profile; baseline noise is reported separately, not subtracted.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def load(path):
    if path.is_dir():
        candidates = list(path.glob("*.png"))
        if len(candidates) != 1:
            raise ValueError(f"Expected one image in {path}")
        path = candidates[0]
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255


def gaussian(image):
    coordinates = np.arange(-5, 6)
    weights = np.exp(-(coordinates**2) / (2 * 1.5**2))
    weights /= weights.sum()
    height, width = image.shape
    padded = np.pad(image, ((0, 0), (5, 5)), mode="reflect")
    horizontal = sum(weight * padded[:, k:k + width] for k, weight in enumerate(weights))
    padded = np.pad(horizontal, ((5, 5), (0, 0)), mode="reflect")
    return sum(weight * padded[k:k + height] for k, weight in enumerate(weights))


def compare(reference, candidate, *, filter_fn=gaussian):
    if reference.shape != candidate.shape or min(reference.shape[:2]) < 11:
        raise ValueError("Matched RGB images at least 11x11 required")
    if not np.isfinite(reference).all() or not np.isfinite(candidate).all():
        raise ValueError("Non-finite image")
    luma = np.array([.2126, .7152, .0722])
    a, b = reference @ luma, candidate @ luma
    ma, mb = filter_fn(a), filter_fn(b)
    va = np.maximum(0, filter_fn(a*a) - ma*ma)
    vb = np.maximum(0, filter_fn(b*b) - mb*mb)
    covariance = filter_fn(a*b) - ma*mb
    ssim = ((2*ma*mb + .01**2) * (2*covariance + .03**2) /
            ((ma*ma + mb*mb + .01**2) * (va + vb + .03**2)))
    error = np.abs(reference**2.2 - candidate**2.2)
    # Preserve partial edge-tile denominators without tens of thousands of
    # individual Python slices/reductions for each full-resolution comparison.
    rows = np.arange(0, error.shape[0], 8)
    columns = np.arange(0, error.shape[1], 8)
    sums = np.add.reduceat(np.add.reduceat(error.sum(axis=2), rows, axis=0), columns, axis=1)
    counts = np.minimum(8, error.shape[0]-rows)[:, None] * np.minimum(8, error.shape[1]-columns)[None, :] * 3
    tiles = (sums/counts).ravel()
    result = {"ssim_gaussian_luma": float(ssim[5:-5, 5:-5].mean()),
              "mean_linear_rgb_error": float(error.mean()),
              "p99_tile_linear_rgb_error": float(np.quantile(tiles, .99)),
              "worst_tile_linear_rgb_error": float(tiles.max()),
              "peak_linear_rgb_error": float(error.max())}
    result["moderate_pass"] = (result["ssim_gaussian_luma"] >= .98 and
                               result["mean_linear_rgb_error"] <= .01 and
                               result["p99_tile_linear_rgb_error"] <= .04)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = compare(load(args.reference), load(args.candidate))
    text = json.dumps(result, indent=2)
    if args.output:
        with args.output.open("x") as output:
            output.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
