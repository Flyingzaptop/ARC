"""Recompute complete-frame comparison, preserving every slow sample in the window."""
import csv,json,statistics,sys
from pathlib import Path
def stats(v):
    if not v:return None
    s=sorted(v)
    def pct(p):
        x=(len(s)-1)*p;i=int(x);return s[i]+(s[min(i+1,len(s)-1)]-s[i])*(x-i)
    return dict(n=len(v),mean=statistics.mean(v),median=statistics.median(v),p95=pct(.95),p99=pct(.99),maximum=max(v))
def analyze(root):
    result={}
    for run in json.loads((root/'runs.json').read_text()):
        if run['code']:raise ValueError('failed process')
        rows=list(csv.DictReader((root/run['name']/'frames.csv').open()))
        for r in rows:
            if None in r or any(v is None for v in r.values()):raise ValueError('malformed CSV')
            r['ms']=float(r['ms']);r['elapsed_ms']=float(r['elapsed_ms']);r['frame']=int(r['frame'])
        # Packet/GPU events carry the actual originating frame; publication is deferred.
        frame_times={r['frame']:r['elapsed_ms'] for r in rows if r['event']=='CPU processing through submission ms'}
        steady=[r for r in rows if 6000<=frame_times.get(r['frame'],r['elapsed_ms'])<=19000]
        events={k:stats([r['ms'] for r in steady if r['event']==k]) for k in sorted({r['event'] for r in steady})}
        cadence=[r['ms'] for r in steady if r['event']=='Controlled Present interval']
        counts={k:sum(r['ms'] for r in rows if r['event']==k) for k in ['Indirect records replaced','Indirect validated words','Indirect validated commands','Indirect commands','Indirect busy frame fallback','Indirect CPU metadata wait ms']}
        if run['name'].startswith('indirect') and not counts['Indirect records replaced']:raise ValueError('no real replacement')
        if run['name']=='oracle' and not counts['Indirect validated commands']:raise ValueError('no command validation')
        if counts['Indirect CPU metadata wait ms']!=0:raise ValueError('metadata wait')
        result[run['name']]={'steady':events,'counts':counts,'cadence_hz':1000/statistics.mean(cadence) if cadence else None,'slow_over_50':sum(v>50 for v in cadence),'slow_over_100':sum(v>100 for v in cadence),'all_run_cadence':stats([r['ms'] for r in rows if r['event']=='Controlled Present interval'])}
    return result
if __name__=='__main__':
    r=analyze(Path(sys.argv[1]));Path(sys.argv[2]).write_text(json.dumps(r,indent=2)+'\n')
    for name,v in r.items():print(name,v['cadence_hz'],v['counts'])
