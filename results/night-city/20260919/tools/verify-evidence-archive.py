"""Verify checksum manifests against actual Git blobs, not working-tree EOL rules."""
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repository")
parser.add_argument("artifact")
parser.add_argument("--revision", default="HEAD")
args = parser.parse_args()
root = args.artifact.replace("\\", "/").strip("/")

def blob(path):
    return subprocess.check_output(["git", "-C", args.repository, "show", f"{args.revision}:{path}"])

manifest = json.loads(blob(root + "/checksums.json"))
for entry in manifest:
    path = root + "/" + entry["file"].replace("\\", "/")
    data = blob(path)
    if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest().upper() != entry["sha256"].upper():
        raise SystemExit(f"Git blob checksum mismatch: {path}")
print(f"PASS: {len(manifest)} archived Git blobs match their original byte checksums")
