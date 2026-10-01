"""Package the experimental frontend without external engine binaries/assets."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def package(root, output, binary_source, expected_sha256, meter_source):
    actual = hashlib.sha256((root / 'build/Release/arc2-frontend.dll').read_bytes()).hexdigest()
    if actual != expected_sha256.lower():
        raise ValueError('normal DLL differs from requested measured build')
    files = {
        'arc2-frontend.dll': 'build/Release/arc2-frontend.dll',
        'arc2-frontend-meter.dll': 'build/Release/arc2-frontend-meter.dll',
        'arc2-native-lab.exe': 'build/Release/arc2-native-lab.exe',
        'arc2-binding-native.exe': 'build/Release/arc2-binding-native.exe',
        'include/arc/arc2/bootstrap.hpp': 'include/arc/arc2/bootstrap.hpp',
        'include/arc/arc2/bootstrap_loader.hpp': 'include/arc/arc2/bootstrap_loader.hpp',
        'README.md': 'docs/ARC2_PACKAGE.md',
        'ARC2_RESULTS.md': 'docs/ARC2_RESULTS.md',
        'ARC2_ARCHITECTURE.md': 'docs/ARC2_ARCHITECTURE.md',
        'ARC2_IR.md': 'docs/ARC2_IR.md',
        'ARC2_MIGRATION.md': 'docs/ARC2_MIGRATION.md',
        'ARC2_TEST_MATRIX.md': 'docs/ARC2_TEST_MATRIX.md',
        'ARC2_VALIDATION_LIMITS.md': 'docs/ARC2_VALIDATION_LIMITS.md',
    }
    entries = []
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, relative in files.items():
            data = (root / relative).read_bytes()
            archive.writestr(name, data)
            entries.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        manifest = dict(schema='arc2-experimental-package-v1',
                        packaging_commit=subprocess.check_output(
                            ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
                        normal_binary_source=binary_source,
                        diagnostic_binary_source=meter_source,
                        note='Use binary hashes for measured-version identity; packaging commit can include later documentation and diagnostic-only changes.',
                        files=entries)
        archive.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise ValueError('package ZIP integrity check failed')
        for entry in entries:
            if hashlib.sha256(archive.read(entry['path'])).hexdigest() != entry['sha256']:
                raise ValueError('package member hash mismatch: ' + entry['path'])
    print(json.dumps(dict(path=str(output), bytes=output.stat().st_size,
                         sha256=hashlib.sha256(output.read_bytes()).hexdigest())))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--binary-source', required=True)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--meter-source', required=True)
    args = parser.parse_args()
    package(Path(__file__).resolve().parents[2], args.output.resolve(),
            args.binary_source, args.expected_sha256, args.meter_source)
