"""Assemble an offline Windows x64 ARC package; never install into system Python."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PYTHON_URL = 'https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip'
PYTHON_SHA256 = 'd1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf'
PACKAGES = ['numpy==2.2.6', 'opencv-python-headless==4.12.0.88', 'pillow==11.3.0']


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def extract(archive, directory):
    # Dependency archives may contain package paths, never parent traversal.
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            destination = (directory / member.filename).resolve()
            if not destination.is_relative_to(directory.resolve()):
                raise ValueError('Archive member outside package')
        zipped.extractall(directory)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--dxc', required=True, type=Path)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/Release')
    parser.add_argument('--downloads', type=Path, default=ROOT / 'build/package-downloads')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    downloads = args.downloads.resolve()
    downloads.mkdir(parents=True, exist_ok=True)
    for name in ['arc-launcher.exe', 'arc-dx12-probe-launch.exe', 'arc-dx12-probe.dll', 'arc-shader-tool.exe']:
        shutil.copy2(args.build / name, output / name)
    scripts = output / 'scripts'
    scripts.mkdir()
    for name in ['optimizer-live-quality.py', 'optimizer-quality.py']:
        shutil.copy2(ROOT / 'scripts' / name, scripts / name)
    dxc = output / 'runtime/dxc'
    dxc.mkdir(parents=True)
    for name in ['dxcompiler.dll', 'dxil.dll']:
        shutil.copy2(args.dxc / name, dxc / name)
    licenses = output / 'licenses'
    licenses.mkdir()
    for path in args.dxc.parent.parent.glob('LICENSE*'):
        shutil.copy2(path, licenses / ('dxc-' + path.name))
    shutil.copy2(ROOT / 'third_party/minhook/LICENSE.txt', licenses / 'minhook-LICENSE.txt')
    shutil.copy2(ROOT / 'third_party/json/LICENSE.MIT', licenses / 'json-LICENSE.MIT')
    python_archive = downloads / PYTHON_URL.rsplit('/', 1)[1]
    if not python_archive.exists():
        urllib.request.urlretrieve(PYTHON_URL, python_archive)
    if sha(python_archive) != PYTHON_SHA256:
        raise ValueError('Python archive does not match the published Python.org checksum')
    runtime = output / 'runtime/python'
    runtime.mkdir()
    extract(python_archive, runtime)
    wheels = downloads / 'wheels-cp313'
    wheels.mkdir(exist_ok=True)
    subprocess.run([sys.executable, '-m', 'pip', 'download', '--disable-pip-version-check',
                    '--only-binary=:all:', '--platform', 'win_amd64', '--python-version', '3.13',
                    '--implementation', 'cp', '--abi', 'cp313', '--dest', str(wheels), *PACKAGES], check=True)
    dependencies = []
    selected_wheels = []
    for requirement in PACKAGES:
        name, version = requirement.split('==')
        matches = list(wheels.glob(name.replace('-', '_') + '-' + version + '-*.whl'))
        if len(matches) != 1:
            raise ValueError('Expected exactly one pinned wheel: ' + requirement)
        selected_wheels.extend(matches)
    for wheel in selected_wheels:
        name, version = wheel.name.split('-')[:2]
        with urllib.request.urlopen(f'https://pypi.org/pypi/{name}/{version}/json', timeout=30) as response:
            release = json.load(response)
        published = next(item for item in release['urls'] if item['filename'] == wheel.name)
        if sha(wheel) != published['digests']['sha256']:
            raise ValueError('Wheel checksum mismatch: ' + wheel.name)
        extract(wheel, runtime / 'Lib/site-packages')
        dependencies.append({'file': wheel.name, 'sha256': sha(wheel), 'url': published['url']})
    # Isolated embedded Python, independent of PATH and user site packages.
    (runtime / 'python313._pth').write_text('python313.zip\n.\nLib/site-packages\nimport site\n')
    subprocess.run([str(runtime / 'python.exe'), '-I', '-c',
                    'import numpy,cv2,PIL; assert numpy.__version__=="2.2.6"; assert cv2.__version__=="4.12.0"; print("Packaged quality dependencies OK")'], check=True)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip())
    shutil.copy2(ROOT / 'docs/CONNECTION_HANDOFF_REPORT.md', output / 'CONNECTION_HANDOFF_REPORT.md')
    if args.report:
        shutil.copy2(args.report, output / 'REPORT.md')
    (output / 'README.txt').write_text(
        'ARC development package\n'
        'Run arc-launcher.exe. Choose a DX12 executable or an explicit running process ID.\n'
        'Default objective: maximum verified FPS with image-quality checks; native resolution.\n'
        'Ctrl+Alt+F9 toggles the click-through diagnostic overlay. Ctrl+Alt+F10 stops optimization.\n'
        'Intermediate-pass maps are not object-visibility or gaze measurements.\n'
        'Logs: %LOCALAPPDATA%/ARC/sessions. See REPORT.md when supplied.\n'
        'This is an experimental build, not a guarantee of a gain in every DX12 game.\n', encoding='utf-8')
    manifest = {'schema': 1, 'revision': revision, 'source_dirty': dirty, 'release_accepted': False,
                'python': {'url': PYTHON_URL, 'sha256': PYTHON_SHA256}, 'dependencies': dependencies,
                'files': {str(file.relative_to(output)): sha(file) for file in output.rglob('*') if file.is_file()}}
    (output / 'package-manifest.json').write_text(json.dumps(manifest, indent=2))
    print(output)


if __name__ == '__main__':
    main()
