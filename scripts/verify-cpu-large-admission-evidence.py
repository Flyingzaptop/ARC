"""Replay only bounded existing evidence; no observer, process attachment or trace."""
import argparse,csv,hashlib,json,collections,statistics
from pathlib import Path
import pefile
from cpu_contract_events import event_records
from cpu_admission_conditions import Semantics,Access,before_dispatch

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('study',type=Path);p.add_argument('image',type=Path);p.add_argument('screen',type=Path);p.add_argument('evidence',type=Path);a=p.parse_args()
    life=json.loads((a.evidence/'lifecycle.json').read_text());inner=json.loads((a.evidence/'inner-economics.json').read_text())
    assert digest(a.image)==life['image_sha256']
    pe=pefile.PE(str(a.image))
    for region in life['regions']:
        data=pe.get_data(region['begin_rva'],region['end_rva']-region['begin_rva'])
        assert data.hex()==region['bytes'] and hashlib.sha256(data).hexdigest()==region['sha256']
    directory=a.study/'final-consumer'/'candidate-00'
    assert digest(directory/'events.bin')==inner['short_consumer_tail']['sha256']
    meta=json.loads((directory/'capture.json').read_text());hist=collections.Counter()
    for e in event_records(directory,meta):
        if e['rip']-meta['main_base']==0x1ca130:hist[e['registers'][8]&0xffffffff]+=1
    assert sum(hist.values())==inner['short_consumer_tail']['recorded_loop_visits']
    assert {str(k):v for k,v in hist.items()}==inner['short_consumer_tail']['mask_histogram']
    assert digest(a.screen)==inner['parent_screen']['sha256']
    rows=[];metrics=collections.defaultdict(list)
    with a.screen.open(newline='') as f:
        for row in csv.DictReader(f):
            if float(row['elapsed_ms'])>=6000 and row['event'].startswith('Resident previous'):
                rows.append(row);metrics[row['event']].append(float(row['ms']))
    with (a.evidence/'parent-timing-excerpt.csv').open(newline='') as f:assert rows==list(csv.DictReader(f))
    for name,v in metrics.items():assert inner['parent_screen']['metrics'][name]=={'samples':len(v),'mean':statistics.mean(v),'max':max(v)}
    sort=next(x for x in life['chain'] if x['phase']=='sort')
    current,_=before_dispatch(Semantics(code_generation_matches=True),Access(begin=sort['observed_begin'],size=sort['observed_bytes']))
    assert current==json.loads((a.evidence/'current-admission.json').read_text())['result']
    print('Verified image, 6 static regions, short consumer tail, parent timing and separate admission axes')
if __name__=='__main__':main()
