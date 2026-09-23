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
    def test_pid_reuse_preserves_first_lifetime_and_internal_start(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture / "manifest.json").write_text(json.dumps({
                "target_pid": 42,
                "target_image": "C:\\game.exe",
                "target_creation_utc": "2026-09-23T10:00:00.100000+00:00",
            }), encoding="utf-8")
            (capture / "trace-stats.txt").write_text(
                "Start time (UTC) : 2026/09/23:10:00:00.0000000\n"
                "Total # Lost Buffers : 0\nTotal # Lost Events : 0\n", encoding="utf-8")
            (capture / "events.txt").write_text("""BeginHeader
EndHeader
P-Start, 50000, game.exe (42), 1
SampledProfile, 60000, game.exe (42), 6, 0x101234
P-End, 70000, game.exe (42), 1
P-Start, 100000, game.exe (42), 1
I-Start, 110000, game.exe (42), 0x100000, 0x103000, abc, 123, 0x100000, C:\\game.exe
SampledProfile, 200000, game.exe (42), 7, 0x101234
SampledProfile, 300000, game.exe (42), 7, 0x101456
SampledProfile, 400000, game.exe (42), 7, 0x101999
SampledProfile, 500000, game.exe (42), 7, 0x101999
P-End, 500000, game.exe (42), 1
P-Start, 600000, other.exe (42), 1
I-Start, 610000, other.exe (42), 0x200000, 0x203000, def, 456, 0x200000, C:\\other.exe
SampledProfile, 700000, other.exe (42), 8, 0x201234
SampledProfile, 800000, other.exe (42), 8, 0x201456
SampledProfile, 900000, other.exe (42), 8, 0x201999
""", encoding="utf-8")
            result = MODULE.analyze(capture)
            self.assertEqual(result["counts"]["samples"], 3)
            self.assertEqual(result["counts"]["module_loads"], 1)
            self.assertEqual(result["process_lifetime"]["end_event_offset_us"], 500000)
            self.assertTrue(result["process_lifetime"]["pid_reused"])
            self.assertEqual(result["regions"][0]["sample_hits"], 3)
            self.assertTrue(all("other.exe" not in row["module_path"] for row in result["regions"]))
            events = (capture / "events.txt").read_text(encoding="utf-8")
            events = events.replace("P-Start, 50000", "P-Start, 98000")
            events = events.replace("SampledProfile, 60000", "SampledProfile, 98500")
            events = events.replace("P-End, 70000", "P-End, 99000")
            (capture / "events.txt").write_text(events, encoding="utf-8")
            ambiguous = MODULE.analyze(capture)
            self.assertEqual(ambiguous["regions"], [])
            self.assertIn("target_process_start_ambiguous", ambiguous["missing_evidence"])

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
