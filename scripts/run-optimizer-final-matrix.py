"""Three alternating original / neutral / automatic owned-renderer comparisons.

Every child renderer has the runner's explicit initialization/measurement/process
limits. Do not rebuild binaries, run other GPU jobs, or change power settings
during this matrix. Bring each visible renderer forward during its warmup.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('sdk',type=Path)
parser.add_argument('output',type=Path)
parser.add_argument('--dll',required=True,type=Path)
parser.add_argument('--compiler',required=True,type=Path)
parser.add_argument('--rounds',type=int,default=3)
args=parser.parse_args()
if not 1<=args.rounds<=3: parser.error('One to three bounded rounds required')
root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
cache=root/'cache'
runner=Path(__file__).with_name('run-cauldron-benchmark.py')
files=[args.dll.resolve(),args.dll.resolve().parent/'arc-shader-tool.exe',args.sdk.resolve()/'bin/FFX_BRIXELIZER_GI_DX12.exe']
def hashes():
    return {str(p):hashlib.file_digest(p.open('rb'),'sha256').hexdigest() for p in files}
frozen=hashes()
def bytes_on_disk():
    return sum(p.stat().st_size for p in cache.rglob('*') if p.is_file()) if cache.exists() else 0
def status(value):
    temp=root/'matrix-status.tmp';temp.write_text(json.dumps(value,indent=2));temp.replace(root/'matrix-status.json')
(root/'matrix-manifest.json').write_text(json.dumps(dict(binary_hashes=frozen,rounds=args.rounds,
    order=['original','neutral','automatic'],initialization_seconds=120,measurement_seconds=60,
    cache=str(cache),first_automatic_cache='cold',later_automatic_cache='warm'),indent=2))
completed=[]
for repeat in range(1,args.rounds+1):
    for mode in ('original','neutral','automatic'):
        if hashes()!=frozen: raise RuntimeError('A tested binary changed during the matrix')
        case=root/f'{repeat:02d}-{mode}'
        command=[sys.executable,str(runner),str(args.sdk.resolve()),str(case),
                 '--initialization-seconds','120','--measurement-seconds','60','--visible','--max-start-temperature','60']
        if mode!='original':command+=['--dll',str(args.dll.resolve()),'--compiler',str(args.compiler.resolve()),'--mode','compute-off','--cache',str(cache)]
        if mode=='automatic':command+=['--auto-target','60']
        before=bytes_on_disk()
        status(dict(phase='running',case=str(case),round=repeat,mode=mode,completed=completed))
        result=subprocess.run(command)
        after=bytes_on_disk()
        (root/f'{case.name}-cache-size.json').write_text(json.dumps(dict(before_bytes=before,after_bytes=after)))
        if result.returncode:
            status(dict(phase='failed',case=str(case),exit_code=result.returncode,completed=completed))
            raise SystemExit(result.returncode)
        completed.append(str(case))
status(dict(phase='complete',completed=completed,binary_hashes=frozen))
print(json.dumps(dict(matrix=str(root),completed=len(completed))),flush=True)
