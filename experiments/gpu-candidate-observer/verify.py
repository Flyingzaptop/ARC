"""Post-hoc source oracle. Kept outside the automatic pipeline."""
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);result=json.loads((root/'result.json').read_text());events=[json.loads(x) for x in (root/'events.jsonl').read_text().splitlines()];creation={e['resource']:e for e in events if e['event']=='create'};oracle=[json.loads(x) for x in (root/'oracle.jsonl').read_text().splitlines()];matches=[]
for b in result['bindings']:
    if b['status']!='confirmed':continue
    identity=creation[b['resource']]['native_identity']
    for r in b['discovery']['relations']:
        if r['confirmation']!='confirmed_on_new_write' or r['subsample_of'] is not None:continue
        if any(o['native_identity']==identity and o['cpu_address']==int(r['cpu_region'])+r['cpu_offset'] and all(o[k]==r[k] for k in ['cpu_stride','gpu_stride','gpu_offset','width']) for o in oracle):matches.append({'resource':b['resource'],'relation':r,'oracle_label':'center','selected_without_oracle':True})
if not matches:raise RuntimeError('no independent oracle match')
Path(sys.argv[2]).write_text(json.dumps({'matches':matches},indent=2)+'\n');print('oracle confirmed automatically selected resource and held-out relation')
