"""Independently recompute bounded experiment decisions from raw timings."""
import argparse,csv,json,statistics
from pathlib import Path

def analyze(root):
    result={'scope':'isolated exact-order sort, not automatic offload or game FPS','series':{},'gpu_invocations':0,'record_comparisons':0}
    for run in json.loads((root/'runs.json').read_text()):
        if run['returncode']!=0:raise ValueError('Failed run')
        directory=root/run['name'];rows=list(csv.DictReader((directory/'timings.csv').open()))
        count=int((directory/'stdout.txt').read_text().split('records=')[1].split()[0])
        for row in rows:
            if any(int(row[k]) for k in ['mismatch_records','pending_tasks','gpu_error']):raise ValueError('Invalid result')
            if row['mode']=='gpu':
                result['gpu_invocations']+=1;result['record_comparisons']+=count
                pieces=sum(float(row[k]) for k in ['prep_ms','record_ms','submit_ms','wait_ms','consume_ms'])
                if abs(pieces-float(row['full_ms']))>.001:raise ValueError('Wall-time decomposition does not add up')
        if not run['name'].startswith('real-series'):continue
        if run['debug_layer']:raise ValueError('Diagnostic layer in performance series')
        selected=[r for r in rows if r['warmup']=='0'];cpu=[float(r['full_ms']) for r in selected if r['mode']=='cpu'];gpu=[r for r in selected if r['mode']=='gpu']
        values={name:[float(r[name]) for r in gpu] for name in ['full_ms','gpu_sort_ms','gpu_upload_ms','gpu_init_ms','gpu_return_ms','gpu_root_ms','prep_ms','record_ms','submit_ms','wait_ms','consume_ms']}
        cm=statistics.median(cpu);gm=statistics.median(values['full_ms'])
        result['series'][run['name']]={'cpu_samples':len(cpu),'gpu_samples':len(gpu),'cpu_median_ms':cm,'cpu_min_ms':min(cpu),'cpu_max_ms':max(cpu),'gpu':{k:{'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v)} for k,v in values.items()},'full_gpu_cpu_ratio':gm/cm,'all_gpu_slower_than_all_cpu':min(values['full_ms'])>max(cpu)}
    if len(result['series'])!=2:raise ValueError('Incomplete comparison')
    result['profitable_in_this_experiment']=all(s['full_gpu_cpu_ratio']<1 for s in result['series'].values())
    result['develop_ownership_for_this_sort']=result['profitable_in_this_experiment']
    hardware=[];lost=0
    with (root/'hardware.csv').open() as f:
        for row in csv.DictReader(f):
            row={k.strip():v.strip() if v else None for k,v in row.items()}
            if any(v is None for v in row.values()):lost+=1;continue
            hardware.append(row)
    result['hardware']={'complete_samples':len(hardware),'incomplete_samples':lost,'scope':'whole experiment, including compilation/validation; not per-kernel attribution','power_limit_known':False}
    for field in ['power.draw [W]','temperature.gpu','clocks.current.graphics [MHz]','clocks.current.memory [MHz]']:
        v=[float(row[field].split()[0]) for row in hardware]
        result['hardware'][field]={'min':min(v),'max':max(v),'mean':statistics.mean(v)} if v else None
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    j=analyze(a.directory);a.output.write_text(json.dumps(j,indent=2)+'\n',encoding='utf-8');print(json.dumps({k:j[k] for k in ['gpu_invocations','record_comparisons','profitable_in_this_experiment','develop_ownership_for_this_sort']}))
