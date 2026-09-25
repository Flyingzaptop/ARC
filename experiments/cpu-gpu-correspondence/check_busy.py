import csv,json,sys
from pathlib import Path
rows=list(csv.DictReader(Path(sys.argv[1]).open()))
def total(event):return sum(float(r['ms']) for r in rows if r['event']==event)
release=next(float(r['elapsed_ms']) for r in rows if r['event']=='Busy test GPU hold ms')
result={'held_versions':total('Busy test held versions'),'verified_versions':total('Busy test verified version'),'verified_bytes':total('Busy test verified bytes'),'fallbacks':total('Indirect busy frame fallback'),'metadata_wait_ms':total('Indirect CPU metadata wait ms'),'commands_after_release':sum(float(r['ms']) for r in rows if r['event']=='Indirect commands' and float(r['elapsed_ms'])>release+100),'hold_ms':total('Busy test GPU hold ms')}
assert result['held_versions']==3 and result['verified_versions']==3
assert result['fallbacks']>0 and result['metadata_wait_ms']==0 and result['commands_after_release']>0
Path(sys.argv[2]).write_text(json.dumps(result,indent=2)+'\n');print('three live GPU versions protected; fallback and resumption passed')
