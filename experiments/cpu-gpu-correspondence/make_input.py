"""Remove fixture oracle/field metadata from the learner input."""
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);frames=[]
for i in range(2):
    m=json.loads((root/f'audit.{i}.windows.json').read_text())
    frames.append({'frame':m['frame'],'gpu':f'audit.{i}.gpu.bin','cpu':[{'id':str(r['address']),'file':r['file']} for r in m['regions']]})
(root/'input.json').write_text(json.dumps({'schema':1,'frames':frames},indent=2)+'\n')
