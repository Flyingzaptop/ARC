"""Check captured records and expose a concrete tie-order counterexample."""
import argparse,json,collections
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();hyp=json.loads((a.directory/'key-hypotheses.json').read_text())['hypotheses']
if not hyp:raise SystemExit('No key hypothesis')
bits=hyp[0]['projection']
def key(record):
 x=int.from_bytes(record,'little');return sum(((1 if b==-2 else 0 if b==-1 else (x>>b)&1)<<i) for i,b in enumerate(bits))
b=(a.directory/'input-span.bin').read_bytes();v=(a.directory/'output-span.bin').read_bytes();before=[b[i:i+16] for i in range(0,len(b),16)];after=[v[i:i+16] for i in range(0,len(v),16)];stable=sorted(before,key=key)
result={'records':len(before),'record_bytes_hypothesis':16,'distinct_keys':len({key(x) for x in before}),'stable_key_sort_differs_at_positions':sum(x!=y for x,y in zip(stable,after)),'same_record_multiset':collections.Counter(before)==collections.Counter(after),'first_difference':next((i for i,(x,y) in enumerate(zip(stable,after)) if x!=y),None),'scope':'Counterexample for stable key-sort substitution on this exact input; not a general algorithm proof or permission to reorder ties.'}
(a.directory/'tie-order-counterexample.json').write_text(json.dumps(result,indent=2));print(result)
