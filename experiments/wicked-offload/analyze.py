"""Compare application CPU submission cadence; never label it GPU/display completion."""
import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


def stats(values):
    v = sorted(values)
    if not v:
        return None
    return dict(count=len(v), mean=statistics.mean(v), p50=statistics.median(v),
                p95=v[int(.95*(len(v)-1))], p99=v[int(.99*(len(v)-1))],
                maximum=max(v), above50=sum(x>50 for x in v), above100=sum(x>100 for x in v))


def analyze(path):
    all_rows = list(csv.DictReader(path.open(encoding='utf-8-sig')))
    rows = [r for r in all_rows if 6000 <= float(r['elapsed_ms']) < 15000]
    submit = [r for r in rows if r['event'] == 'SubmitCommandLists']
    series = [(float(b['elapsed_ms'])/1000, float(b['elapsed_ms'])-float(a['elapsed_ms']))
              for a, b in zip(submit, submit[1:]) if int(b['frame']) == int(a['frame'])+1]
    events = {}
    for event in sorted({r['event'] for r in rows}):
        if event.startswith('Offload') or event in ['Frustum Culling', 'Application Update', 'Application Render', 'Controlled Present interval']:
            events[event] = stats([float(r['ms']) for r in rows if r['event'] == event])
    return dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(), cadence=stats([v for _,v in series]),
                series=series, events=events, total_rows=len(all_rows))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    runs={p.stem:analyze(p) for p in args.directory.glob('*.csv')}
    result={'scope':__doc__, 'window_ms':[6000,15000], 'runs':runs}
    if all(runs.get(n,{}).get('cadence') for n in ['A1-retry','A2','C1','C2']):
        a=[runs[n]['cadence']['mean'] for n in ['A1-retry','A2']]
        c=[runs[n]['cadence']['mean'] for n in ['C1','C2']]
        result['cadence_gate']={'required_ms':max(.5,.05*statistics.mean(a),2*abs(a[0]-a[1])),
                                'conservative_gain_ms':min(a)-max(c),'complete_frame_gate':'not_measured'}
    if all(runs.get(n,{}).get('cadence') for n in ['A2','A3','W1','W2']):
        a=[runs[n]['cadence']['mean'] for n in ['A2','A3']]
        c=[runs[n]['cadence']['mean'] for n in ['W1','W2']]
        result['wide_cadence_gate']={'required_ms':max(.5,.05*statistics.mean(a),2*abs(a[0]-a[1])),
                                    'conservative_gain_ms':min(a)-max(c),'complete_frame_gate':'not_measured'}
    (args.directory/'comparison.json').write_text(json.dumps({**result, 'runs': {name: {k:v for k,v in run.items() if k != 'series'} for name,run in runs.items()}},indent=2)+'\n')
    colors=['#2563eb','#0ea5e9','#ea580c','#dc2626','#9333ea','#16a34a']
    names=[n for n in ['A2','A3','C1','C2','W1','W2'] if n in runs and runs[n]['cadence']]
    svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="650" viewBox="0 0 1000 650"><rect width="1000" height="650" fill="white"/>',
         '<text x="55" y="30" font-family="sans-serif" font-size="20">Wicked: CPU submission intervals (not GPU-completed frame time)</text>']
    for y in [0,10,20,30,40,50,60]:
        yy=500-y*7
        svg.append(f'<path d="M60 {yy} H960" stroke="#ddd"/><text x="10" y="{yy+5}">{y} ms</text>')
    for j,name in enumerate(names):
        points=' '.join(f'{60+(t-6)*100:.1f},{500-min(v,60)*7:.1f}' for t,v in runs[name]['series'])
        svg.append(f'<polyline points="{points}" fill="none" stroke="{colors[j]}" stroke-opacity="0.65" stroke-width="1"/>')
        s=runs[name]['cadence']
        svg.append(f'<text x="{60+(j%2)*450}" y="{550+(j//2)*28}" fill="{colors[j]}" font-family="sans-serif">{name}: mean {s["mean"]:.2f}, p95 {s["p95"]:.2f}, p99 {s["p99"]:.2f} ms</text>')
    svg.append('<text x="60" y="523">6 s</text><text x="925" y="523">15 s</text></svg>')
    (args.directory/'frametimes.svg').write_text('\n'.join(svg))
    for name,run in runs.items():
        if run['cadence']:
            c=run['cadence'];print(name, 'mean/p95/p99:',*(round(c[k],3) for k in ['mean','p95','p99']))


if __name__=='__main__':main()
