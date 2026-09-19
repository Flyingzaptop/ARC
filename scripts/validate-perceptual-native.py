"""Independent Mega F raw-image/timestamp validator (Python standard library only)."""
import argparse
import json
import math
import statistics
import struct
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(root, trial, phase):
    stem = root / f"{trial}-{phase}"
    meta = json.loads(stem.with_suffix(".json").read_text())
    raw = stem.with_suffix(".rgb32f").read_bytes()
    count = meta["width"] * meta["height"] * 3
    require(0 < count <= 3840 * 2160 * 3 and len(raw) == count * 4, "image shape")
    pixels = struct.unpack(f"<{count}f", raw)
    require(all(math.isfinite(x) and 0 <= x <= 1 for x in pixels), "invalid pixels")
    ticks, frequency = meta["ticks"], meta["frequency"]
    require(len(ticks) == 22 and frequency > 0, "timestamp shape")
    durations = [(ticks[i + 1] - ticks[i]) * 1000 / frequency for i in range(4, 22, 2)]
    require(all(math.isfinite(x) and x > 0 for x in durations), "invalid GPU duration")
    require(len(meta["gpu_ms"]) == len(durations), "sample count")
    require(all(abs(a - b) < 1e-9 for a, b in zip(durations, meta["gpu_ms"])), "timestamp conversion")
    return meta, pixels, statistics.median(durations)


def evaluate(root, trial):
    (ma, a, ta), (mb, b, tb), (mc, c, tc) = [load(root, trial, p) for p in range(3)]
    require(all(ma[k] == mb[k] == mc[k] for k in ("width", "height", "state_key", "generation")), "unmatched state")
    require(ma["mip"] == mc["mip"] == 0 and mb["mip"] == (2 if trial == 1 else 3), "physical mip/restore identity")
    drift = [abs(x - z) for x, z in zip(a, c)]
    damage = [max(abs(x - y), abs(z - y)) for x, y, z in zip(a, b, c)]
    require(statistics.mean(drift) <= .0005 and max(drift) <= .004, "reference image drift")
    tile_max = 0
    width, height = ma["width"], ma["height"]
    for y0 in range(0, height, 8):
        for x0 in range(0, width, 8):
            tile = [damage[(y * width + x) * 3 + ch]
                    for y in range(y0, min(y0 + 8, height))
                    for x in range(x0, min(x0 + 8, width)) for ch in range(3)]
            tile_max = max(tile_max, statistics.mean(tile))
    image_ok = statistics.mean(damage) <= .002 and max(damage) <= .04 and tile_max <= .008
    stable = abs(ta - tc) / min(ta, tc) <= .10
    gain = min(ta, tc) - tb
    benefit = stable and gain >= .02 and gain / min(ta, tc) >= .02
    return {"image_accepted": image_ok, "timing_stable": stable,
            "benefit_accepted": benefit, "mean_error": statistics.mean(damage),
            "peak_error": max(damage), "tile_error": tile_max, "gain_ms": gain,
            "before_ms": ta, "modified_ms": tb, "after_ms": tc}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    good, bad = evaluate(args.directory, 1), evaluate(args.directory, 2)
    require(good["image_accepted"] and good["benefit_accepted"], "positive trial failed")
    require(not bad["image_accepted"], "damaging trial unexpectedly accepted")
    summary = json.loads((args.directory / "summary.json").read_text())
    require(summary["positive_status"] == 5 and summary["positive_reason"] == 0,
            "controller did not retain accepted action")
    require(summary["restored"] is True and summary["damaging_action_rejected"] is True, "rollback")
    result = {"schema": 1, "verdict": "PASS", "scope": "controlled native DX12 SRV probe; pixel guard, not human-perception equivalence",
              "positive": good, "negative": bad}
    (args.directory / "independent-acceptance.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
