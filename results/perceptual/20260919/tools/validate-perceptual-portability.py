"""Recompute the independent Microsoft raster renderer probe."""
import argparse
import json
from pathlib import Path
import runpy
native_validator = runpy.run_path(str(Path(__file__).with_name("validate-perceptual-native.py")))
evaluate, require = native_validator["evaluate"], native_validator["require"]

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
args = parser.parse_args()
evidence = evaluate(args.directory, 1, modified_mip=1)
summary = json.loads((args.directory / "summary.json").read_text())
require(summary["renderer"] == "Microsoft D3D12HelloTexture" and summary["restored"] is True, "renderer/restore")
if evidence["image_accepted"] and evidence["benefit_accepted"]:
    require(summary["status"] == 5 and summary["reason"] == 0, "accepted evidence did not match controller")
else:
    require(summary["status"] == 4, "unaccepted action was retained")
    expected = 3 if not evidence["image_accepted"] else 5 if not evidence["timing_stable"] else 6
    require(summary["reason"] == expected, "independent rejection reason mismatch")
result = {"schema": 1, "verdict": "PASS", "scope": "same portable core on independent Microsoft raster renderer; rejection is valid when no gain exists", "evidence": evidence, "controller": summary}
(args.directory / "independent-acceptance.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
