"""Package a relocatable ARC2 matrix with every raw frame CSV.

Includes compact SHA-identified IR summaries for every frontend arm and one
complete compressed IR snapshot for each frontend mode.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path, PureWindowsPath


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("matrix_dir", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--config", type=Path, action="append", default=[])
    parser.add_argument("--allow-invalid-arms", action="store_true",
                        help="Preserve explicitly invalid arms with absent native data; never changes their verdict")
    args = parser.parse_args()
    root = args.matrix_dir.resolve()
    if args.archive.exists():
        raise FileExistsError(f"Use a fresh archive path: {args.archive}")
    partial = args.archive.with_name(args.archive.name + ".partial")
    if partial.exists():
        raise FileExistsError(f"Stale partial archive exists: {partial}")
    subprocess.run([sys.executable,
                    str(Path(__file__).with_name("summarize-counterbalanced.py")),
                    str(root / "matrix.json"), "--output", str(root / "summary.json")],
                   check=True, stdout=subprocess.DEVNULL)
    matrix = json.loads((root / "matrix.json").read_text(encoding="utf-8-sig"))
    summary = json.loads((root / "summary.json").read_text(encoding="utf-8-sig"))
    order = matrix.get("order", [])
    allowed = (["native", "passthrough", "observe", "optimize",
                "optimize", "observe", "passthrough", "native"],
               ["native", "passthrough", "passthrough", "native"])
    if order not in allowed or len(matrix["runs"]) != matrix["rounds"] * len(order):
        raise ValueError("Incomplete or malformed matrix order")
    if len(summary["runs"]) != len(matrix["runs"]) or summary["workload"] != matrix["workload"]:
        raise ValueError("Summary does not match matrix")
    runs = []
    run_modes = {}
    for index, item in enumerate(matrix["runs"]):
        if item["mode"] != order[index % len(order)]:
            raise ValueError("Matrix mode sequence changed")
        run = root / PureWindowsPath(item["run_json"]).parent.name
        if not run.is_dir() or not (run / "run.json").exists():
            raise FileNotFoundError(f"Required run artifact absent: {run}")
        # An invalid arm can have no native CSV (for example, recorder startup
        # failed while frontend rows were written). Preserve that absence in the
        # archive; measured arms still require the independent native record.
        if (not (run / "native.frames.csv").exists() and
                (summary["runs"][index]["status"] == "measured" or not args.allow_invalid_arms)):
            raise FileNotFoundError(f"Arm lacks native frames (invalid preservation needs explicit opt-in): {run}")
        if item["mode"] != "native":
            for required in ("arc2.json", "arc2.json.frames.csv",
                             "arc2.json.frames.meta.json", "arc2-ir.zip"):
                if not (run / required).exists():
                    raise FileNotFoundError(f"Required frontend run artifact absent: {run / required}")
        runs.append(run)
        run_modes[run] = item["mode"]
    if len(set(runs)) != len(runs):
        raise ValueError("Matrix references the same run directory twice")
    for config in args.config:
        if not config.is_file():
            raise FileNotFoundError(config)
    for document in (matrix, summary):
        for run in document["runs"]:
            source = PureWindowsPath(run["run_json"])
            run["run_json"] = f"{source.parent.name}/run.json"
    selected_ir_modes: set[str] = set()
    args.archive.parent.mkdir(parents=True, exist_ok=True)
    manifest = []

    def add(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
        archive.writestr(name, data)
        manifest.append({"path": name, "bytes": len(data), "sha256": digest(data)})

    with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9, allowZip64=True) as archive:
        add(archive, "matrix.json", (json.dumps(matrix, indent=2) + "\n").encode())
        add(archive, "summary.json", (json.dumps(summary, indent=2) + "\n").encode())
        for run in runs:
            for name in ("run.json", "native.frames.csv", "arc2.json.frames.csv",
                         "arc2.json.frames.meta.json", "arc2.json.analysis.json"):
                path = run / name
                if path.exists():
                    add(archive, f"{run.name}/{name}", path.read_bytes())
            dump = run / "arc2.json"
            if dump.exists():
                compact = run / "ir-summary.json"
                subprocess.run([sys.executable,
                                str(Path(__file__).with_name("summarize-ir.py")),
                                str(dump), "--output", str(compact)], check=True,
                               stdout=subprocess.DEVNULL)
                if json.loads(compact.read_text())["dump_sha256"] != digest(dump.read_bytes()):
                    raise ValueError(f"IR summary hash differs from dump: {dump}")
                add(archive, f"{run.name}/ir-summary.json", compact.read_bytes())
                mode = run_modes[run]
                zipped = run / "arc2-ir.zip"
                if mode not in selected_ir_modes:
                    with zipfile.ZipFile(zipped) as inner:
                        names = inner.namelist()
                        if len(names) != 1 or Path(names[0]).name != "arc2.json":
                            raise ValueError(f"Unexpected IR ZIP layout: {zipped}")
                        if digest(inner.read(names[0])) != digest(dump.read_bytes()):
                            raise ValueError(f"IR ZIP does not match raw dump: {zipped}")
                    add(archive, f"{run.name}/arc2-ir.zip", zipped.read_bytes())
                    selected_ir_modes.add(mode)
        expected_ir_modes = set(order) - {"native"}
        if selected_ir_modes != expected_ir_modes:
            raise ValueError("Missing a full IR representative for a frontend mode")
        for config in args.config:
            add(archive, f"configs/{config.name}", config.read_bytes())
        add(archive, "archive-manifest.json",
            (json.dumps({"schema": "arc2-evidence-archive-v1", "files": manifest}, indent=2) + "\n").encode())
    partial.rename(args.archive)
    print(json.dumps({"archive": str(args.archive), "bytes": args.archive.stat().st_size,
                      "sha256": digest(args.archive.read_bytes()),
                      "selected_ir_modes": sorted(selected_ir_modes)}))


if __name__ == "__main__":
    main()
