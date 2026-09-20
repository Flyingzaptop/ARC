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
    coverage = float((inside & consistent).mean())
    reference_check = metrics["compare"](warp_a, warp_b)
    predicted = warp_a*(1-fraction)+warp_b*fraction
    quality = metrics["compare"](predicted, candidate)
    motion = float(np.quantile(np.linalg.norm(forward, axis=2), .99))
    confident = (informative and coverage >= .95 and motion <= 64 and reference_check["ssim_gaussian_luma"] >= .99 and
                 reference_check["mean_linear_rgb_error"] <= .002)
    return {"schema": 1, "reference_kind": "motion_interpolated_originals", "same_state_replay": False,
            "flow_method": "opencv_dis_medium", "flow_width": size[0], "opencv_version": cv2.__version__,
            "matched_reference": confident, "alignment_coverage": coverage, "motion_p99_pixels": motion,
            "informative_reference": informative,
            "reference_check": reference_check, "quality": quality,
            "accepted_quality": confident and quality["moderate_pass"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path); parser.add_argument("candidate", type=Path); parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    a, am = image(args.before); b, bm = image(args.candidate); c, cm = image(args.after)
    fraction = .5
    if am and bm and cm:
        times = [m["capture_qpc"] for m in (am, bm, cm)]
        if not times[0] < times[1] < times[2]:
            raise ValueError("Capture ordering")
        fraction = (times[1]-times[0])/(times[2]-times[0])
    result = assess(a, b, c, fraction)
    with args.output.open("x") as output:
        json.dump(result, output, indent=2)
    print(json.dumps({k: result[k] for k in ("matched_reference", "accepted_quality", "alignment_coverage")}))


if __name__ == "__main__":
    main()
