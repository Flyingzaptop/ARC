"""Adversarial checks for independent full-frame image and timing validation."""
import json
from pathlib import Path
import runpy
import tempfile
import unittest
import numpy as np

api = runpy.run_path(str(Path(__file__).resolve().parents[1] / "scripts/validate-full-frame-x2.py"))


class FullFrameEvidence(unittest.TestCase):
    def test_late_waypoint_damage_is_protected(self):
        reference = np.full((16, 16, 3), .4)
        modified = reference.copy()
        modified[12:, 12:, :] = .5
        self.assertFalse(api["compare"](reference, modified, reference)["pass"])

    def test_pixel_damage_is_not_diluted(self):
        a = np.full((1080, 1920, 3), .4)
        b = a.copy()
        b[0, 0, 0] = .8
        self.assertFalse(api["compare"](a, b, a)["pass"])

    def test_reference_drift_rejects(self):
        a = np.full((1080, 1920, 3), .4)
        c = a.copy()
        c[10, 10, 0] += .01
        self.assertFalse(api["compare"](a, a, c)["pass"])

    def test_full_frame_timing_is_not_sum_of_medians(self):
        with tempfile.TemporaryDirectory(prefix="arc-x2-validator-") as directory:
            path = Path(directory) / "frames.json"
            record = {"width": 1920, "height": 1080, "frequency": 1000,
                      "frames": [{"wall_ms": 35, "ticks": [0, 5, 30, 31, 33]} for _ in range(21)]}
            path.write_text(json.dumps(record))
            _, metrics, _ = api["timing"](path)
            self.assertEqual(metrics["gpu_p50_ms"], 33)
            self.assertEqual(metrics["wall_p50_ms"], 35)
            self.assertEqual(metrics["pass_p50_ms"], [5, 25, 1, 2])
            record["frames"][0]["ticks"][2] = 1
            path.write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                api["timing"](path)
            record["frames"] = record["frames"][:1]
            path.write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                api["timing"](path)


if __name__ == "__main__":
    unittest.main()
