"""Independent pixel and temporal comparison of testbed GPU capture folders."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def captures(folder: Path) -> dict[str, Path]:
    files = {p.name: p for p in folder.glob("*.png")}
    if not files:
        raise ValueError(f"No PNG captures in {folder}")
    return files


def pixels(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.int16)


def error(a: np.ndarray, b: np.ndarray, tile: int) -> dict[str, float | int]:
    if a.shape != b.shape:
        raise ValueError(f"Image shape differs: {a.shape} vs {b.shape}")
    difference = np.abs(a - b)
    tile_max = 0.0
    for y in range(0, a.shape[0], tile):
        for x in range(0, a.shape[1], tile):
            tile_max = max(tile_max, float(difference[y:y + tile, x:x + tile].mean()))
    return {
        "mean_absolute_channel_error": float(difference.mean()),
        "max_absolute_channel_error": int(difference.max()),
        "max_tile_mean_error": tile_max,
        "changed_channels": int(np.count_nonzero(difference)),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("modified", type=Path)
    parser.add_argument("--after", type=Path)
    parser.add_argument("--tile", type=int, default=32)
    parser.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "WIDTH", "HEIGHT"),
                        help="also compare a documented scene ROI; full image metrics remain")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.tile < 1:
        raise ValueError("tile must be positive")
    before = captures(args.before)
    modified = captures(args.modified)
    after = captures(args.after) if args.after else None
    if before.keys() != modified.keys() or (after and before.keys() != after.keys()):
        raise ValueError("Capture frame names/count differ")
    frames = []
    previous_before = previous_modified = None
    for name in sorted(before):
        a = pixels(before[name])
        b = pixels(modified[name])
        entry = {
            "frame": name,
            "before_sha256": hashlib.sha256(before[name].read_bytes()).hexdigest(),
            "modified_sha256": hashlib.sha256(modified[name].read_bytes()).hexdigest(),
            "modified_error": error(a, b, args.tile),
        }
        if args.crop:
            x, y, width, height = args.crop
            if x < 0 or y < 0 or width < 1 or height < 1 or x + width > a.shape[1] or y + height > a.shape[0]:
                raise ValueError("Crop lies outside capture")
            entry["scene_crop_error"] = error(a[y:y + height, x:x + width],
                                               b[y:y + height, x:x + width], args.tile)
        if after:
            c = pixels(after[name])
            entry["baseline_drift"] = error(a, c, args.tile)
            if args.crop:
                entry["baseline_crop_drift"] = error(a[y:y + height, x:x + width],
                                                      c[y:y + height, x:x + width], args.tile)
        if previous_before is not None:
            entry["temporal_difference_error"] = error(
                a - previous_before, b - previous_modified, args.tile
            )
        frames.append(entry)
        previous_before, previous_modified = a, b
    result = {
        "schema": "arc2-capture-comparison-v1",
        "before": str(args.before.resolve()),
        "modified": str(args.modified.resolve()),
        "after": str(args.after.resolve()) if args.after else None,
        "tile_size": args.tile,
        "scene_crop_xywh": args.crop,
        "frames": len(frames),
        "exact_images": sum(f["before_sha256"] == f["modified_sha256"] for f in frames),
        "max_absolute_channel_error": max(f["modified_error"]["max_absolute_channel_error"] for f in frames),
        "max_tile_mean_error": max(f["modified_error"]["max_tile_mean_error"] for f in frames),
        "max_scene_crop_channel_error": max(
            (f.get("scene_crop_error", {}).get("max_absolute_channel_error", 0) for f in frames),
            default=0,
        ),
        "max_temporal_difference_error": max(
            (f.get("temporal_difference_error", {}).get("max_absolute_channel_error", 0) for f in frames),
            default=0,
        ),
        "per_frame": frames,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in (
        "frames", "exact_images", "max_absolute_channel_error",
        "max_tile_mean_error", "max_temporal_difference_error"
    )}))


if __name__ == "__main__":
    main()
