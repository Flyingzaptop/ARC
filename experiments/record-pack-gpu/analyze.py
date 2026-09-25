"""Recompute bounded live experiment from complete CSVs, without dropping slow frames."""
import csv,json,statistics,sys
from pathlib import Path
def percentile(v,p):
    s=sorted(v);x=(len(s)-1)*p;i=int(x);return s[i]+(s[min(i+1,len(s)-1)]-s[i])*(x-i)
def stats(v):
    return {'n':len(v),'mean':statistics.mean(v),'median':statistics.median(v),'p95':percentile(v,.95),'p99':percentile(v,.99),'max':max(v)} if v else None
def analyze(root):
    runs=json.loads((root/'runs.json').read_text());result={}
    for run in runs:
        if run['code']:raise ValueError('failed process')
        rows=list(csv.DictReader((root/run['name']/'frames.csv').open()))
        for r in rows:
            if None in r or any(v is None for v in r.values()):raise ValueError('malformed CSV')
            r['ms']=float(r['ms']);r['elapsed_ms']=float(r['elapsed_ms']);r['frame']=int(r['frame'])
        steady=[r for r in rows if 6000<=r['elapsed_ms']<=19000]
        events={k:stats([r['ms'] for r in steady if r['event']==k]) for k in sorted({r['event'] for r in steady})}
        cadence=[r['ms'] for r in steady if r['event']=='Controlled Present interval']
        if run['name']!='oracle' and not cadence:raise ValueError('missing cadence')
        result[run['name']]={'steady_6_to_19_seconds':events,'cadence_hz':1000/statistics.mean(cadence) if cadence else None,'cadence_scope':'CPU interval between Present invocations, not display FPS','slow_over_50':sum(v>50 for v in cadence),'slow_over_100':sum(v>100 for v in cadence),'all_active_cadence':stats([r['ms'] for r in rows if r['elapsed_ms']>=4000 and r['event']=='Controlled Present interval']),'validated_records':sum(r['ms'] for r in rows if r['event']=='Packet validated records'),'replaced_records':sum(r['ms'] for r in rows if r['event']=='Packet replaced records'),'fallbacks':sum(r['ms'] for r in rows if r['event']=='Packet fallback')}
    return result
if __name__=='__main__':
    r=analyze(Path(sys.argv[1]));Path(sys.argv[2]).write_text(json.dumps(r,indent=2)+'\n')
    for name,v in r.items():print(name,v['cadence_hz'],v['validated_records'],v['replaced_records'],v['fallbacks'])
