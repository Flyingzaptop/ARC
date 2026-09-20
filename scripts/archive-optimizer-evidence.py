"""Archive selected generated measurements, excluding shader code and caches."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

parser=argparse.ArgumentParser()
parser.add_argument('evidence_root',type=Path)
parser.add_argument('destination',type=Path)
parser.add_argument('cases',nargs='+')
args=parser.parse_args()
root=args.evidence_root.resolve();destination=args.destination.resolve()
destination.mkdir(parents=True,exist_ok=False)
index={'status':'checkpoint_not_complete','source_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
       'shader_bytecode_or_ir_published':False,'archives':[]}
for name in args.cases:
    directory=(root/name).resolve()
    if not directory.is_relative_to(root) or not directory.is_dir():raise ValueError('Evidence case must be inside the supplied root')
    archive=destination/(directory.relative_to(root).as_posix().replace('/','--')+'.zip')
    count=0
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as output:
        for path in sorted(directory.rglob('*')):
            relative=path.relative_to(directory)
            if any(part in ('cache','shader-cache') or part.endswith('.shaders') for part in relative.parts):continue
            if not path.is_file() or path.suffix.lower() not in ('.json','.jsonl','.png','.pixels','.tiles'):continue
            if not path.resolve().is_relative_to(directory):raise ValueError('Evidence symlink outside case')
            output.write(path,relative.as_posix());count+=1
    index['archives'].append({'name':archive.name,'files':count,'bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()})
(destination/'index.json').write_text(json.dumps(index,indent=2)+'\n')
print(json.dumps(index,indent=2))
