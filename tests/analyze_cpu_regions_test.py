import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "analyze-cpu-regions.py"
SPEC = importlib.util.spec_from_file_location("analyze_cpu_regions", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AnalyzeCpuRegionsTests(unittest.TestCase):
    def test_same_trace_lifetime_module_offset_and_evidence_gaps(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture / "manifest.json").write_text(json.dumps({
                "target_pid": 42,
                "target_creation_utc": "2026-09-23T10:00:00+00:00",
            }), encoding="utf-8")
            (capture / "trace-stats.txt").write_text(
                "Start time (UTC) : 2026/09/23:10:00:00.0000000\n"
                "Total # Lost Buffers : 0\nTotal # Lost Events : 0\n", encoding="utf-8")
            (capture / "events.txt").write_text("""BeginHeader
SampledProfile, TimeStamp, Process, Thread, PC
EndHeader
P-DCStart, 0, game.exe (42), 1
I-DCStart, 0, game.exe (42), 0x100000, 0x103000, abc, 123, 0x100000, C:\\game.exe
SampledProfile, 100000, game.exe (42), 7, 0x101234
ReadyThread, 110000, game.exe (42), 7, game.exe (42), 9
CSwitch, 120000, game.exe (42), 9, 1, 1, 0, 0, Idle (0), 0, 1, 1, Running, Executive
SampledProfile, 220000, game.exe (42), 7, 0x101456
SampledProfile, 330000, game.exe (42), 7, 0x101999
SampledProfile, 340000, other.exe (43), 8, 0x101999
I-End, 400000, game.exe (42), 0x100000, 0x103000
SampledProfile, 450000, game.exe (42), 7, 0x101999
""", encoding="utf-8")
            result = MODULE.analyze(capture)
            self.assertEqual(result["counts"]["samples"], 4)
            self.assertEqual(result["counts"]["unattributed_samples"], 1)
            self.assertEqual(result["regions"][0]["offset_begin"], "0x1000")
            self.assertEqual(result["regions"][0]["sample_hits"], 3)
            self.assertEqual(result["scheduling"]["target_runnable_us_by_thread"], {9: 10000})
            self.assertIn("same_etl_frame_boundaries_missing", result["missing_evidence"])
            self.assertTrue(result["event_loss"]["event_loss_verified"])
            self.assertEqual(result["regions"][0]["admission"],
                             "declined_no_region_semantics_or_dependency_proof")


if __name__ == "__main__":
    unittest.main()
