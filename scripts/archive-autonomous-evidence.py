"""Archive numerical/image evidence without vendor shader bytecode or IR."""
import argparse
import gzip
import hashlib
import json
import shutil
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('matrix',type=Path)
parser.add_argument('reference',type=Path)
parser.add_argument('automatic',type=Path)
parser.add_argument('quality',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
def copy(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    if src.suffix=='.jsonl':
        dst=dst.with_suffix('.jsonl.gz');dst.write_bytes(gzip.compress(src.read_bytes(),mtime=0))
    else:shutil.copy2(src,dst)
for name in ('summary.json','matrix-manifest.json','matrix-status.json'):
    copy(args.matrix/name,out/name)
for case in sorted(args.matrix.glob('[0-9]*-*')):
    if not case.is_dir():continue
    for name in ('manifest.json','frames.jsonl','gpu-samples.json'):
        copy(case/name,out/'runs'/case.name/name)
    if (case/'automatic/events.jsonl').exists():
        copy(case/'automatic/events.jsonl',out/'runs'/case.name/'events.jsonl')
        for trial in (case/'automatic').glob('trial-*'):
            decision=trial/'decision.json'
            if not decision.exists():continue
            data=json.loads(decision.read_text())
            if data.get('decision_reason')!='accepted_net_gain_and_quality':continue
            for name in ('decision.json','quality.json','submission-provenance.json','frame-state.json'):
                if (trial/name).exists():copy(trial/name,out/'accepted'/case.name/trial.name/name)
copy(args.quality,out/'matched-quality.json')
for item in json.loads(args.quality.read_text()):
    name=f"oracle-{item['scene_frame']}.png"
    copy(args.reference/name,out/'quality/reference'/name)
    copy(args.automatic/name,out/'quality/automatic'/name)
for directory,label in ((args.reference,'reference'),(args.automatic,'automatic')):
    for name in ('manifest.json','oracle.jsonl'):
        copy(directory/name,out/'quality'/label/name)
files={str(p.relative_to(out)).replace('\\','/'):{'bytes':p.stat().st_size,'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest()} for p in out.rglob('*') if p.is_file() and p.name!='evidence-manifest.json'}
(out/'evidence-manifest.json').write_text(json.dumps({'schema':1,'vendor_bytecode_included':False,'files':files},indent=2))
print(json.dumps({'files':len(files),'bytes':sum(v['bytes'] for v in files.values()),'output':str(out)}))
