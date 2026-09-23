"""Source-assisted async chain diagnostics; CPU cadence is not GPU-completed FPS."""
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path
import sys
from analyze import analyze, stats

root=Path(sys.argv[1])
runs={}
for path in root.glob('*.csv'):
    run=analyze(path)
    groups=defaultdict(list)
    for row in csv.DictReader(path.open(encoding='utf-8-sig')):
        if 6000<=float(row['elapsed_ms'])<15000:
            groups[int(row['frame'])].append(row)
    metrics=defaultdict(list)
    for rows in groups.values():
        starts=[r for r in rows if r['event']=='Chain in flight packets']
        waits=[r for r in rows if r['event']=='Chain consumer wait ms']
        if len(starts)!=1 or len(waits)!=int(float(starts[0]['ms'])):
            continue
        # Slot's old "independent gap" event includes earlier slot wait/commit.
        # Use FIRST consumer entry minus LAST submit marker instead.
        metrics['independent_CPU_interval_ms'].append(float(waits[0]['elapsed_ms'])-float(waits[0]['ms'])-float(starts[0]['elapsed_ms']))
        events=defaultdict(float)
        for r in rows:
            if r['event'].startswith('Chain'):events[r['event']]+=float(r['ms'])
        for k,v in events.items():metrics[k].append(v)
    run['chain_per_frame']={k:stats(v) for k,v in metrics.items()}
    runs[path.stem]=run
gates={}
if all(runs.get(n,{}).get('cadence') for n in ['A1','A2']):
    a=[runs[n]['cadence']['mean'] for n in ['A1','A2']]
    required=max(.5,.05*statistics.mean(a),2*abs(a[0]-a[1]))
    for prefix in ['P','Q']:
        if all(runs.get(prefix+str(i),{}).get('cadence') for i in [1,2]):
            b=[runs[prefix+str(i)]['cadence']['mean'] for i in [1,2]]
            gates[prefix]={'required_ms':required,'conservative_gain_ms':min(a)-max(b),'scope':'CPU submission cadence only; full GPU completion gate unavailable'}
result={'runs':{n:{k:v for k,v in r.items() if k!='series'} for n,r in runs.items()},'gates':gates}
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
for name,run in runs.items():
    if run['cadence']:
        print(name,{k:round(run['cadence'][k],3) for k in ['mean','p95','p99']})
colors=['#2563eb','#0ea5e9','#ea580c','#dc2626','#9333ea','#16a34a']
svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="650"><rect width="1000" height="650" fill="white"/>',
     '<text x="55" y="30" font-family="sans-serif" font-size="20">Async chain: CPU submission intervals (not GPU completion)</text>']
for y in range(0,61,10):
    yy=500-y*7;svg.append(f'<path d="M60 {yy} H960" stroke="#ddd"/><text x="10" y="{yy+5}">{y} ms</text>')
for j,n in enumerate(['A1','A2','P1','P2','Q1','Q2']):
    if n not in runs or not runs[n]['cadence']:continue
    points=' '.join(f'{60+(t-6)*100:.1f},{500-min(v,60)*7:.1f}' for t,v in runs[n]['series'])
    svg.append(f'<polyline points="{points}" fill="none" stroke="{colors[j]}" stroke-opacity=".65"/>')
    s=runs[n]['cadence'];svg.append(f'<text x="{60+(j%2)*450}" y="{550+j//2*28}" fill="{colors[j]}" font-family="sans-serif">{n}: mean {s["mean"]:.2f}, p95 {s["p95"]:.2f}, p99 {s["p99"]:.2f} ms</text>')
svg.append('<text x="60" y="523">6 s</text><text x="925" y="523">15 s</text></svg>')
(root/'frametimes.svg').write_text('\n'.join(svg))
