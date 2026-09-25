"""Independent fixture oracle, invoked after discovery; never imported by the learner."""
import json,sys
from pathlib import Path
result=json.loads(Path(sys.argv[1]).read_text());root=Path(sys.argv[2]);oracle=json.loads((root/'audit.0.oracle.json').read_text())
primary=[r for r in result['relations'] if r['subsample_of'] is None]
matched=[r for r in primary if int(r['cpu_region'])+r['cpu_offset']==oracle['cpu_address'] and all(r[k]==oracle[k] for k in ['cpu_stride','gpu_stride','gpu_offset','width'])]
if len(matched)!=1:raise RuntimeError('independent oracle did not confirm unique center mapping')
report={'primary_relations':len(primary),'subsample_aliases':len(result['relations'])-len(primary),'oracle_matches':len(matched),'semantic_label_from_oracle_only':'center','inferred':matched[0],'automatic_offload':False}
Path(sys.argv[3]).write_text(json.dumps(report,indent=2)+'\n');print('independent oracle confirmed learned layout')
