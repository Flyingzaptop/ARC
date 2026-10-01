"""Set the same unthrottled presentation setting for all Cauldron arms."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "build/arc2-testbeds/FidelityFX-1.1.4"
for path in (
    SDK / "framework/cauldron/framework/config/cauldronconfig.json",
    SDK / "bin/configs/cauldronconfig.json",
):
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if "Cauldron" not in data or "Presentation" not in data["Cauldron"]:
        raise RuntimeError(f"Unexpected Cauldron config schema: {path}")
    data["Cauldron"]["Presentation"]["Vsync"] = False
    path.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
    print(f"{path}: sha256={hashlib.sha256(path.read_bytes()).hexdigest()}")

# The same static Toyshop glTF remains a heavy GI/PBR scene while eliminating
# animation-clock drift in independent A/B/A image checks.
for path in (
    SDK / "samples/brixelizergi/config/brixelizergiconfig.json",
    SDK / "bin/configs/brixelizergiconfig.json",
):
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    content = data["FidelityFX Brixelizer GI"]["Content"]
    static = "../media/Toyshop_TeddyGI/Toyshop_Teddy_static/Toyshop_Teddy_static.gltf"
    if static not in content["Scenes"]:
        raise RuntimeError(f"Static Toyshop scene missing: {path}")
    content["Scenes"] = [static]
    path.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
    print(f"{path}: sha256={hashlib.sha256(path.read_bytes()).hexdigest()}")
