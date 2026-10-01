"""Small synthetic rejection tests; no graphics process or GPU is launched."""
import csv
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SCRIPTS = REPO / "scripts" / "arc2"
ORDER = ["native", "passthrough", "passthrough", "native"]
IR = (json.dumps({"objects": [], "resources": [], "descriptors": [], "work": [],
                  "submissions": [], "presents": [], "total_work": 0,
                  "dropped": 0, "history_truncated": False,
                  "incomplete": False}) + "\n").encode()


def write_csv(path, fields, rows):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class MatrixScriptsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="arc2-matrix-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "matrix"
        self.root.mkdir()
        runs = []
        self.dirs = []
        for index, mode in enumerate(ORDER, 1):
            folder = self.root / f"r01-{index:02d}-{mode}"
            folder.mkdir()
            self.dirs.append(folder)
            run = {"testbed": "diligent", "workload": "fixture", "mode": mode,
                   "executable_sha256": "exe-sha", "frontend_sha256": None if mode == "native" else "dll-sha",
                   "working_directory": "C:/fixture", "arguments": ["--scene", "fixed"],
                   "environment": {}, "arc_commit": "commit", "qpc_frequency": 1000,
                   "process_start_qpc": 0, "measurement_start_qpc": 5000,
                   "measurement_end_qpc": 15000, "eligible_for_timing_analysis": True,
                   "native_present_rows": 5, "present_rows": 0 if mode == "native" else 5}
            (folder / "run.json").write_text(json.dumps(run), encoding="utf-8")
            times = [1000, 3000, 5200, 10000, 14700]
            native = [{"index": n, "begin_qpc": end - 10, "end_qpc": end,
                       "hresult": 0, "flags": 0, "successful_present": 1,
                       "qpc_frequency": 1000} for n, end in enumerate(times)]
            write_csv(folder / "native.frames.csv", list(native[0]), native)
            if mode != "native":
                frontend = [{"swapchain": 1, "begin_qpc": end - 10,
                             "native_end_qpc": end, "wrapper_end_qpc": end + 1,
                             "hresult": 0, "flags": 0, "successful_present": 1,
                             "qpc_frequency": 1000} for end in times]
                write_csv(folder / "arc2.json.frames.csv", list(frontend[0]), frontend)
                (folder / "arc2.json.frames.meta.json").write_text(
                    json.dumps({"rows": 5, "dropped": 0}), encoding="utf-8")
                (folder / "arc2.json").write_bytes(IR)
                with zipfile.ZipFile(folder / "arc2-ir.zip", "w") as archive:
                    archive.writestr("arc2.json", IR)
            runs.append({"round": 1, "order_index": index, "mode": mode,
                         "run_json": f"{folder.name}/run.json"})
        matrix = {"schema": "arc2-counterbalanced-v1", "workload": "fixture",
                  "rounds": 1, "warmup_seconds": 5, "seconds": 15,
                  "order": ORDER, "runs": runs}
        (self.root / "matrix.json").write_text(json.dumps(matrix), encoding="utf-8")

    def run_summary(self, expect_ok=True):
        result = subprocess.run([sys.executable, str(SCRIPTS / "summarize-counterbalanced.py"),
                                 str(self.root / "matrix.json"), "--output",
                                 str(self.root / "summary.json")], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, expect_ok, result.stderr)
        if expect_ok:
            return json.loads((self.root / "summary.json").read_text(encoding="utf-8"))

    def run_package(self, expect_ok=True, allow_invalid=False):
        archive = self.base / "packed.zip"
        result = subprocess.run([sys.executable, str(SCRIPTS / "package-matrix.py"),
                                 str(self.root), str(archive)] +
                                (["--allow-invalid-arms"] if allow_invalid else []),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, expect_ok, result.stderr)
        return archive

    def update_run(self, index, **changes):
        path = self.dirs[index] / "run.json"
        run = json.loads(path.read_text(encoding="utf-8"))
        run.update(changes)
        path.write_text(json.dumps(run), encoding="utf-8")

    def test_baseline_and_relocated_archive(self):
        summary = self.run_summary()
        self.assertTrue(summary["complete_measured_brackets"])
        self.assertEqual([run["status"] for run in summary["runs"]], ["measured"] * 4)
        archive = self.run_package()
        extracted = self.base / "extracted"
        with zipfile.ZipFile(archive) as package:
            package.extractall(extracted)
            names = set(package.namelist())
            self.assertIn("r01-02-passthrough/arc2-ir.zip", names)
            self.assertIn("r01-02-passthrough/ir-summary.json", names)
            ir_summary = json.loads(package.read("r01-02-passthrough/ir-summary.json"))
            self.assertEqual(ir_summary["dump_sha256"], hashlib.sha256(IR).hexdigest())
        relocated = subprocess.run([sys.executable, str(SCRIPTS / "summarize-counterbalanced.py"),
                                    str(extracted / "matrix.json"), "--root", str(extracted),
                                    "--output", str(extracted / "replayed.json")],
                                   capture_output=True, text=True)
        self.assertEqual(relocated.returncode, 0, relocated.stderr)
        self.assertTrue(json.loads((extracted / "replayed.json").read_text())["complete_measured_brackets"])

    def test_rejects_frontend_only_warmup_and_dropped_rows(self):
        path = self.dirs[1] / "arc2.json.frames.csv"
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fields, rows = reader.fieldnames, list(reader)
        write_csv(path, fields, rows[:2])
        self.update_run(1, present_rows=2)
        (self.dirs[1] / "arc2.json.frames.meta.json").write_text('{"rows":2,"dropped":0}')
        self.assertEqual(self.run_summary()["runs"][1]["status"], "invalid")
        write_csv(path, fields, rows)
        self.update_run(1, present_rows=5)
        (self.dirs[1] / "arc2.json.frames.meta.json").write_text('{"rows":5,"dropped":1}')
        self.assertEqual(self.run_summary()["runs"][1]["status"], "invalid")

    def test_rejects_status_test_flag_and_out_of_order(self):
        path = self.dirs[0] / "native.frames.csv"
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fields, rows = reader.fieldnames, list(reader)
        for row in rows[2:]:
            row["hresult"] = "142213121"  # Positive DXGI status, despite successful_present=1.
        write_csv(path, fields, rows)
        self.assertEqual(self.run_summary()["runs"][0]["status"], "invalid")
        for row in rows[2:]:
            row["hresult"], row["flags"] = "0", "1"  # DXGI_PRESENT_TEST.
        write_csv(path, fields, rows)
        self.assertEqual(self.run_summary()["runs"][0]["status"], "invalid")
        for row in rows[2:]:
            row["flags"] = "0"
        rows[-1], rows[-2] = rows[-2], rows[-1]
        write_csv(path, fields, rows)
        self.assertEqual(self.run_summary()["runs"][0]["status"], "invalid")

    def test_rejects_order_and_cross_arm_identity(self):
        path = self.root / "matrix.json"
        matrix = json.loads(path.read_text(encoding="utf-8"))
        matrix["runs"][1]["mode"] = "optimize"
        path.write_text(json.dumps(matrix), encoding="utf-8")
        self.run_summary(expect_ok=False)
        matrix["runs"][1]["mode"] = "passthrough"
        path.write_text(json.dumps(matrix), encoding="utf-8")
        for field, value in (("executable_sha256", "other-exe"),
                             ("frontend_sha256", "other-dll"),
                             ("arc_commit", "other-commit"),
                             ("arguments", ["--scene", "other"])):
            original = json.loads((self.dirs[1] / "run.json").read_text())[field]
            self.update_run(1, **{field: value})
            self.run_summary(expect_ok=False)
            self.update_run(1, **{field: original})

    def test_rejects_binary_or_head_change_during_run(self):
        for changes in ({"executable_sha256_after": "other-exe"},
                        {"frontend_sha256_after": "other-dll"},
                        {"arc_commit_after": "other-commit"},
                        {"binary_hash_stable": False},
                        {"repo_head_stable": False}):
            path = self.dirs[1] / "run.json"
            original = path.read_text(encoding="utf-8")
            self.update_run(1, **changes)
            self.run_summary(expect_ok=False)
            path.write_text(original, encoding="utf-8")

    def test_windows_literal_run_and_output_paths_relocate(self):
        matrix_path = self.root / "matrix.json"
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        for index, folder in enumerate(self.dirs):
            matrix["runs"][index]["run_json"] = f"C:\\old-host\\matrix\\{folder.name}\\run.json"
            self.update_run(index, testbed="cauldron",
                            arguments=[f"path=C:\\old-host\\matrix\\{folder.name}"])
        matrix_path.write_text(json.dumps(matrix), encoding="utf-8")
        summary = self.run_summary()
        self.assertTrue(summary["complete_measured_brackets"])
        archive = self.run_package()
        with zipfile.ZipFile(archive) as package:
            self.assertIn("r01-02-passthrough/run.json", package.namelist())

    def test_rejects_missing_native_csv(self):
        (self.dirs[0] / "native.frames.csv").unlink()
        self.run_package(expect_ok=False)
        # A missing native CSV is found before archive creation.
        self.assertFalse((self.base / "packed.zip").exists())

    def test_invalid_archive_requires_explicit_opt_in(self):
        (self.dirs[0] / "native.frames.csv").unlink()
        archive = self.run_package(allow_invalid=True)
        with zipfile.ZipFile(archive) as package:
            summary = json.loads(package.read("summary.json"))
            self.assertEqual(summary["runs"][0]["status"], "invalid")
            self.assertNotIn("r01-01-native/native.frames.csv", package.namelist())

    def test_rejects_missing_frontend_dump(self):
        (self.dirs[1] / "arc2.json").unlink()
        self.run_package(expect_ok=False)
        self.assertFalse((self.base / "packed.zip").exists())

    def test_rejects_ir_zip_mismatched_to_dump(self):
        with zipfile.ZipFile(self.dirs[1] / "arc2-ir.zip", "w") as archive:
            archive.writestr("arc2.json", b'{"different":true}\n')
        self.run_package(expect_ok=False)
        self.assertFalse((self.base / "packed.zip").exists())

    def test_regenerates_stale_ir_summary(self):
        (self.dirs[1] / "ir-summary.json").write_text('{"dump_sha256":"stale"}')
        archive = self.run_package()
        with zipfile.ZipFile(archive) as package:
            summary = json.loads(package.read("r01-02-passthrough/ir-summary.json"))
            self.assertEqual(summary["dump_sha256"], hashlib.sha256(IR).hexdigest())

    def test_rejects_reused_runner_directory(self):
        output = self.base / "stale-output"
        output.mkdir()
        (output / "native.frames.csv").write_text("stale")
        command = ["pwsh", "-NoProfile", "-File", str(SCRIPTS / "run-testbed.ps1"),
                   "-Testbed", "diligent", "-Workload", "fixture", "-Mode", "native",
                   "-Exe", sys.executable, "-OutputDir", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OutputDir must be fresh", result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
