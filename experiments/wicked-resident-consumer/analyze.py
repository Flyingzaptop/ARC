"""A-B-C/C-B-A analysis. CPU submission cadence is not display FPS."""
import argparse
import csv
import datetime as dt
import hashlib
import json
import re
import statistics
from pathlib import Path

def stats(values):
    v=sorted(values)
    if not v:return None
    return dict(count=len(v),mean=statistics.mean(v),p50=statistics.median(v),p95=v[int(.95*(len(v)-1))],p99=v[int(.99*(len(v)-1))],maximum=max(v),above50=sum(x>50 for x in v),above100=sum(x>100 for x in v))

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();root=args.directory
    runs={};series={}
    for name in ['A1','B1','C1','C2','B2','A2']:
        p=root/(name+'.csv');rows=[r for r in csv.DictReader(p.open(encoding='utf-8-sig')) if 6000<=float(r['elapsed_ms'])<15000]
        submit=[r for r in rows if r['event']=='SubmitCommandLists']
        pairs=[(float(b['elapsed_ms'])/1000,float(b['elapsed_ms'])-float(a['elapsed_ms'])) for a,b in zip(submit,submit[1:]) if int(b['frame'])==int(a['frame'])+1]
        series[name]=pairs
        events={}
        for e in sorted({r['event'] for r in rows}):
            if e.startswith('Resident') or e.startswith('Retired GPU') or e in ['Application Update','RenderPath3D Update','Application Render','Application Compose','SubmitCommandLists']:
                events[e]=stats([float(r['ms']) for r in rows if r['event']==e])
        meta=json.loads((root/(name+'.csv.json')).read_text(encoding='utf-8-sig'))
        launch=dt.datetime.fromisoformat(meta['started']).replace(tzinfo=None)
        hardware={};samples=[]
        for sample in csv.DictReader((root/(name+'-hardware.csv')).open(encoding='utf-8-sig')):
            sample={k.strip():v.strip() for k,v in sample.items()};time=dt.datetime.strptime(sample['timestamp'],'%Y/%m/%d %H:%M:%S.%f')
            if 6<=(time-launch).total_seconds()<15:samples.append(sample)
        for key in ['temperature.gpu','power.draw [W]','clocks.current.graphics [MHz]','utilization.gpu [%]']:
            v=[float(re.search(r'-?\d+(?:\.\d+)?',s[key]).group()) for s in samples if re.search(r'-?\d+(?:\.\d+)?',s[key])]
            hardware[key]={'min':min(v),'max':max(v),'mean':statistics.mean(v)} if v else None
        runs[name]={'cadence_ms':stats([v for _,v in pairs]),'events':events,'hardware_approx_launch_window':hardware,'raw_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'exit_code':meta['exit_code']}
    a=[runs[n]['cadence_ms']['mean'] for n in ['A1','A2']];b=[runs[n]['cadence_ms']['mean'] for n in ['B1','B2']];c=[runs[n]['cadence_ms']['mean'] for n in ['C1','C2']]
    result={'scope':__doc__,'window_seconds':[6,15],'gpu_timing_scope':'retired GPU ranges, not matched to same-row CPU frame; unavailable for original A','runs':runs,
            'comparison':{'A_mean_ms':statistics.mean(a),'B_mean_ms':statistics.mean(b),'C_mean_ms':statistics.mean(c),'interval_reduction_percent':100*(1-statistics.mean(c)/statistics.mean(a)),
                          'cadence_rate_ratio':statistics.mean(a)/statistics.mean(c),'required_gain_ms':max(.5,.05*statistics.mean(a),2*abs(a[0]-a[1])),
                          'conservative_gain_ms':min(a)-max(c),'display_FPS_proven':False}}
    (root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    colors=['#2563eb','#0ea5e9','#ea580c','#dc2626']
    svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="650"><rect width="1000" height="650" fill="white"/>',
         '<text x="55" y="28" font-family="sans-serif" font-size="20">Wicked: original A vs GPU-resident C (CPU submission intervals)</text>']
    for val in range(0,41,10):
        y=490-val*10;svg.append(f'<path d="M60 {y} H960" stroke="#ddd"/><text x="10" y="{y+5}">{val} ms</text>')
    for i,n in enumerate(['A1','A2','C1','C2']):
        points=' '.join(f'{60+(t-6)*100:.1f},{490-min(v,40)*10:.1f}' for t,v in series[n])
        svg.append(f'<polyline points="{points}" fill="none" stroke="{colors[i]}" stroke-opacity=".7" stroke-width="1"/>')
        s=runs[n]['cadence_ms'];svg.append(f'<text x="{60+i%2*450}" y="{550+i//2*30}" fill="{colors[i]}" font-family="sans-serif">{n}: mean {s["mean"]:.2f}, p95 {s["p95"]:.2f}, p99 {s["p99"]:.2f} ms</text>')
    svg.append('<text x="60" y="515">6 s</text><text x="925" y="515">15 s</text><text x="60" y="625" font-family="sans-serif">Rendering outputs verified separately; these are not measured display FPS.</text></svg>')
    (root/'frametimes.svg').write_text('\n'.join(svg))
    print(json.dumps(result['comparison'],indent=2))
if __name__=='__main__':main()
