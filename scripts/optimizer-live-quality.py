"""Confidence-qualified live A/B/A image assessment, outside the render process.

Flow is estimated only between the two ORIGINAL images. Candidate pixels never
influence motion estimation. This is a live proxy, not a same-state replay proof.
Unreliable alignment rejects the trial instead of relaxing quality thresholds.
"""
import argparse
import json
import runpy
import sys
from pathlib import Path
import numpy as np

repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "build/quality-worker"))
import cv2
cv2.setNumThreads(1)
metrics = runpy.run_path(str(repo / "scripts/optimizer-quality.py"))
gaussian_coordinates = np.arange(-5, 6)
gaussian_weights = np.exp(-(gaussian_coordinates**2)/(2*1.5**2))
gaussian_weights /= gaussian_weights.sum()


def fast_gaussian(pixels):
    # Same float64 11x11 separable Gaussian and reflect-without-repeating-edge
    # border as the independent NumPy implementation; no resolution reduction.
    return cv2.sepFilter2D(pixels, cv2.CV_64F, gaussian_weights, gaussian_weights,
                           borderType=cv2.BORDER_REFLECT_101)


def state_fraction(samples):
    """Estimate simulation position from original game constants, not B pixels."""
    if len(samples) != 3 or not samples[0].get("pipeline"):
        return None
    if any(s.get("pipeline") != samples[0]["pipeline"] or s.get("keys") != samples[0].get("keys") for s in samples):
        return None
    if not samples[0]["submission"] < samples[1]["submission"] < samples[2]["submission"]:
        return None
    rows = len(samples[0].get("keys", []))
    if not 1 <= rows <= 64 or any(len(s.get("words", [])) != rows or len(s.get("valid", [])) != rows for s in samples):
        return None
    values = [np.asarray(s["words"], dtype="<u4").view("<f4").astype(np.float64) for s in samples]
    if any(v.shape != (rows, 4) for v in values):
        return None
    a, b, c = values
    valid = np.ones((rows, 4), dtype=bool)
    for s, value in zip(samples, values):
        valid &= np.isfinite(value) & (np.abs(value) <= 1e6)
        valid &= (np.asarray(s["valid"], dtype=np.uint32)[:, None] & (1 << np.arange(4))) != 0
    span = c-a
    valid &= np.abs(span) > np.maximum(1, np.maximum(np.abs(a), np.abs(c)))*1e-6
    ratios = np.divide(b-a, span, out=np.zeros_like(span), where=valid)
    valid &= (ratios > 0) & (ratios < 1)
    if valid.sum() < 4 or np.count_nonzero(valid.any(axis=1)) < 2:
        return None
    observed = ratios[valid]
    fraction = float(np.median(observed))
    spread = float(np.quantile(observed, .75)-np.quantile(observed, .25))
    if not .15 <= fraction <= .85 or spread > .03:
        return None
    return {"fraction": fraction, "interquartile_spread": spread, "components": int(valid.sum()),
            "source": "original_game_float_constants"}


def image(path):
    if path.suffix.lower() != ".json":
        return metrics["load"](path), {}
    metadata = json.loads(path.read_text())
    if (not metadata.get("readback_complete") or not metadata.get("color_space_known") or
            metadata.get("gpu_features_only") or metadata.get("present_hresult") != 0):
        raise ValueError("Readback/color-space contract unavailable")
    if metadata.get("color_space") != 0:
        raise ValueError("Live critic currently admits SDR G22/P709 only")
    width, height = metadata["width"], metadata["height"]
    if not 11 <= width <= 7680 or not 11 <= height <= 4320:
        raise ValueError("Image extent")
    payload = path.parent / metadata["pixel_file"]
    if payload.parent.resolve() != path.parent.resolve():
        raise ValueError("Pixel payload must be local to the capture")
    raw = np.frombuffer(payload.read_bytes(), np.uint8)
    if raw.size != width * height * 4:
        raise ValueError("Image payload size")
    fmt = metadata["dxgi_format"]
    if fmt in (28, 87):
        rgb = raw.reshape(height, width, 4)[:, :, :3]
        if fmt == 87:
            rgb = rgb[:, :, ::-1]
        return rgb.astype(np.float64)/255, metadata
    if fmt == 24:
        packed = raw.view("<u4").reshape(height, width)
        return np.stack([packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 1023], axis=2)/1023, metadata
    raise ValueError("Unsupported display format")


def assess(before, candidate, after, fraction=.5):
    if before.shape != candidate.shape or before.shape != after.shape or not 0 < fraction < 1:
        raise ValueError("Matched images and an interior reference time required")
    if any(not np.isfinite(pixels).all() or pixels.min() < 0 or pixels.max() > 1
           for pixels in (before, candidate, after)):
        raise ValueError("Finite normalized pixels required")
    height, width = before.shape[:2]
    # Preserve small edges at the native 1080p acceptance resolution. DIS gives
    # materially better reference alignment than low-resolution Farneback on
    # the moving fixture; confidence limits remain unchanged.
    scale = min(1, 1920/width)
    size = (max(16, round(width*scale)), max(16, round(height*scale)))
    def gray(pixels):
        value = np.clip((pixels @ np.array([.2126, .7152, .0722]))*255, 0, 255).astype(np.uint8)
        return cv2.resize(value, size, interpolation=cv2.INTER_AREA)
    a, b = gray(before), gray(after)
    informative = float(a.std()) >= 2 and float(b.std()) >= 2
    forward = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM).calc(a, b, None)
    backward = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM).calc(b, a, None)
    forward = cv2.resize(forward, (width, height), interpolation=cv2.INTER_LINEAR)
    backward = cv2.resize(backward, (width, height), interpolation=cv2.INTER_LINEAR)
    forward[:, :, 0] *= width/size[0]; forward[:, :, 1] *= height/size[1]
    backward[:, :, 0] *= width/size[0]; backward[:, :, 1] *= height/size[1]
    x, y = np.meshgrid(np.arange(width, dtype=np.float32), np.arange(height, dtype=np.float32))
    def inverse(flow, t):
        sx, sy = x.copy(), y.copy()
        for _ in range(4):
            sampled = cv2.remap(flow, sx, sy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            sx, sy = x-t*sampled[:, :, 0], y-t*sampled[:, :, 1]
        return sx, sy
    ax, ay = inverse(forward, fraction)
    bx, by = inverse(backward, 1-fraction)
    warp_a = cv2.remap(before, ax, ay, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    warp_b = cv2.remap(after, bx, by, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    fa = cv2.remap(forward, ax, ay, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    back_at_end = cv2.remap(backward, ax+fa[:, :, 0], ay+fa[:, :, 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    consistent = np.linalg.norm(fa+back_at_end, axis=2) <= 1.5
    inside = (ax >= 0) & (ay >= 0) & (ax < width-1) & (ay < height-1) & (bx >= 0) & (by >= 0) & (bx < width-1) & (by < height-1)
    flow_coverage = float((inside & consistent).mean())
    # Motion is not identifiable on flat surfaces (the aperture problem), but
    # their predicted colour can still be identifiable. Accept that evidence
    # only when BOTH original references are locally flat and agree closely.
    # Candidate pixels never contribute to this confidence mask.
    la = (warp_a @ np.array([.2126, .7152, .0722])).astype(np.float32)
    lb = (warp_b @ np.array([.2126, .7152, .0722])).astype(np.float32)
    kernel = np.ones((3, 3), np.uint8)
    flat = ((cv2.dilate(la, kernel)-cv2.erode(la, kernel) <= 2/255) &
            (cv2.dilate(lb, kernel)-cv2.erode(lb, kernel) <= 2/255))
    agreement = np.max(np.abs(warp_a**2.2-warp_b**2.2), axis=2) <= .001
    coverage = float((inside & (consistent | (flat & agreement))).mean())
    reference_check = metrics["compare"](warp_a, warp_b, filter_fn=fast_gaussian)
    predicted = warp_a*(1-fraction)+warp_b*fraction
    quality = metrics["compare"](predicted, candidate, filter_fn=fast_gaussian)
    motion = float(np.quantile(np.linalg.norm(forward, axis=2), .99))
    confident = (informative and coverage >= .95 and motion <= 64 and reference_check["ssim_gaussian_luma"] >= .99 and
                 reference_check["mean_linear_rgb_error"] <= .002)
    return {"schema": 1, "reference_kind": "motion_interpolated_originals", "same_state_replay": False,
            "flow_method": "opencv_dis_medium", "flow_width": size[0], "opencv_version": cv2.__version__,
            "matched_reference": confident, "alignment_coverage": coverage, "motion_p99_pixels": motion,
            "flow_consistency_coverage": flow_coverage,
            "flat_reference_agreement_fraction": float((inside & flat & agreement).mean()),
            "informative_reference": informative,
            "reference_check": reference_check, "quality": quality,
            "accepted_quality": confident and quality["moderate_pass"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path); parser.add_argument("candidate", type=Path); parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--state", type=Path)
    args = parser.parse_args()
    a, am = image(args.before); b, bm = image(args.candidate); c, cm = image(args.after)
    fraction = .5
    if am and bm and cm:
        times = [m["capture_qpc"] for m in (am, bm, cm)]
        if not times[0] < times[1] < times[2]:
            raise ValueError("Capture ordering")
        fraction = (times[1]-times[0])/(times[2]-times[0])
    timing = {"source": "present_qpc" if am and bm and cm else "midpoint", "fraction": fraction}
    if args.state:
        estimated = state_fraction(json.loads(args.state.read_text()))
        if estimated:
            timing = estimated
            fraction = estimated["fraction"]
    result = assess(a, b, c, fraction)
    result["reference_timing"] = timing
    with args.output.open("x") as output:
        json.dump(result, output, indent=2)
    print(json.dumps({k: result[k] for k in ("matched_reference", "accepted_quality", "alignment_coverage")}))


if __name__ == "__main__":
    main()
