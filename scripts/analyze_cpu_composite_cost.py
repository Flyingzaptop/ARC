"""Recompute isolated CPU/GPU route economics from saved raw timing evidence."""
import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


FILES={
    'append': ('append-original.json','append-gather.json','fused-tail.csv','external_native_gather_ms'),
    'word': ('word-original.json','word-gather.json','word-fused-tail.csv','native_gather_ms'),
}
GPU_COMPONENTS=('prep_ms','record_ms','submit_ms','wait_ms','consume_ms','full_ms',
                'gpu_upload_ms','gpu_eval_ms','gpu_local_prefix_ms',
                'gpu_group_prefix_ms','gpu_scatter_ms','gpu_return_ms')


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _native_cpu(path):
    wrapper=json.loads(Path(path).read_text())
    if wrapper.get('returncode')!=0:
        raise ValueError('native original CPU replay failed')
    run=json.loads(wrapper['stdout'])
    if run.get('returncode')!=0:
        raise ValueError('nested native original CPU replay failed')
    sample=json.loads(run['stdout'])
    values=sample.get('warm_samples_ms')
    if (not sample.get('after_snapshot_equal') or not sample.get('warm_comparison_admitted') or
            not isinstance(values,list) or not values or any(not isinstance(x,(int,float)) or x<0 for x in values)):
        raise ValueError('original CPU warm evidence incomplete')
    return sample,values


def _gather(path,count):
    evidence=json.loads(Path(path).read_text())
    if evidence.get('original_input_sha256')!=evidence.get('rerun_input_sha256') or not evidence.get('executable_sha256'):
        raise ValueError('gather executable or packed input identity missing')
    samples=evidence.get('samples')
    if not isinstance(samples,list) or len(samples)<3:
        raise ValueError('three saved gather reruns required')
    gathered=[];verification=[];full=[]
    for sample in samples:
        if sample.get('returncode')!=0 or sample.get('stderr'):
            raise ValueError('gather rerun failed')
        fields=dict(part.split('=',1) for part in sample['stdout'].split() if '=' in part)
        if (int(fields['rows'])!=count or int(fields['first_two_exact'])!=1 or
                int(fields['bounded_snapshot_reads'])!=1 or int(fields['row_bytes'])<=0):
            raise ValueError('gather rows, bounds or replay mismatch')
        gather,verify,all_ms=(float(fields[k]) for k in
                              ('gather_ms','verification_ms','full_prep_ms'))
        if min(gather,verify,all_ms)<0 or abs(gather+verify-all_ms)>0.0002:
            raise ValueError('gather timer decomposition mismatch')
        gathered.append(gather);verification.append(verify);full.append(all_ms)
    return evidence,gathered,verification,full


def _gpu(path,count):
    with Path(path).open(newline='') as source:
        rows=list(csv.DictReader(source))
    if not rows or [int(r['iteration']) for r in rows]!=list(range(-1,20)):
        raise ValueError('fused GPU trial is not warmup plus 20 timed runs')
    if any(int(r['count'])!=count for r in rows):
        raise ValueError('fused GPU count changed')
    for index in (0,len(rows)-1):
        if int(rows[index]['validated'])!=1 or int(rows[index]['mismatch_words'])!=0:
            raise ValueError('fused GPU warmup/final bytes differ')
    timed=rows[1:]
    values={key:[float(r[key]) for r in timed] for key in GPU_COMPONENTS}
    if any(any(x<0 for x in v) for v in values.values()):
        raise ValueError('negative GPU or CPU trial time')
    return rows,values


def analyze(folder,kind,prior_single=None):
    folder=Path(folder)
    if kind not in FILES:raise ValueError('unknown route kind')
    cpu_name,gather_name,gpu_name,cost_key=FILES[kind]
    current=json.loads((folder/'results.json').read_text())
    cpu,cpu_warm=_native_cpu(folder/cpu_name)
    count=cpu['packet_count']
    gather_raw,gather,verification,full_prep=_gather(folder/gather_name,count)
    gpu_rows,gpu=_gpu(folder/gpu_name,count)
    if current.get('count')!=count:
        raise ValueError('saved route count differs from original CPU packet')
    old=(current.get('prior_single_gather_ms',current.get(cost_key))
         if prior_single is None else prior_single)
    if not isinstance(old,(int,float)) or old<0:
        raise ValueError('prior single gather measurement missing')
    cpu_mean,cpu_median=statistics.mean(cpu_warm),statistics.median(cpu_warm)
    gather_mean,gather_median=statistics.mean(gather),statistics.median(gather)
    gpu_mean,gpu_median=statistics.mean(gpu['full_ms']),statistics.median(gpu['full_ms'])
    composed_mean,composed_median=gather_mean+gpu_mean,gather_median+gpu_median
    costs={
        'schema':1,'kind':kind,'count':count,'scope':'isolated nonoverlapped composed route; no frame-impact inference',
        'sources':{name:_sha(folder/name) for name in (cpu_name,gather_name,gpu_name)},
        'original_cpu_warm_samples_ms':cpu_warm,
        'original_cpu_warm_mean_ms':cpu_mean,'original_cpu_warm_median_ms':cpu_median,
        'gather_samples_ms':gather,'gather_mean_ms':gather_mean,'gather_median_ms':gather_median,
        'prior_single_gather_ms':float(old),
        'gather_verification_samples_ms':verification,
        'gather_full_prep_samples_ms':full_prep,
        'fused_full_samples_ms':gpu['full_ms'],'fused_full_mean_ms':gpu_mean,
        'fused_full_median_ms':gpu_median,'fused_full_range_ms':[min(gpu['full_ms']),max(gpu['full_ms'])],
        'fused_component_medians_ms':{key:statistics.median(gpu[key]) for key in GPU_COMPONENTS},
        'composed_nonoverlapped_mean_ms':composed_mean,
        'composed_nonoverlapped_median_ms':composed_median,
        'regression_vs_cpu_warm_mean_ms':composed_mean-cpu_mean,
        'regression_vs_cpu_warm_median_ms':composed_median-cpu_median,
        'decision':('reject_isolated_gpu_route_economics_even_against_cpu_warm_mean'
                    if composed_mean>cpu_mean and composed_median>cpu_median else 'economics_not_decided'),
        'gpu_timing_note':'stage timestamps are within fused wait/full time and are not added again',
        'gather_timing_note':'diagnostic verification is separate from steady gather; no CPU/GPU overlap assumed',
        'correctness':{'warmup_mismatch_words':int(gpu_rows[0]['mismatch_words']),
                       'final_mismatch_words':int(gpu_rows[-1]['mismatch_words'])},
        'packed_input_sha256':gather_raw['rerun_input_sha256'],
        'gather_executable_sha256':gather_raw['executable_sha256'],
    }
    return costs


def write_analysis(folder,kind):
    folder=Path(folder);cost=analyze(folder,kind)
    cost_path=folder/'cost-analysis.json'
    cost_path.write_text(json.dumps(cost,indent=2)+'\n')
    result_path=folder/'results.json';result=json.loads(result_path.read_text())
    key=FILES[kind][3]
    result.update({
        'prior_single_gather_ms':cost['prior_single_gather_ms'],
        key:cost['gather_median_ms'],
        'gather_samples_ms':cost['gather_samples_ms'],
        'gather_mean_ms':cost['gather_mean_ms'],
        'gather_median_ms':cost['gather_median_ms'],
        'gather_verification_samples_ms':cost['gather_verification_samples_ms'],
        'external_gather_source' if kind=='append' else 'native_gather_source':
            'three saved bounded native gather reruns; diagnostic verification excluded',
        'offline_gather_verification_ms':statistics.median(cost['gather_verification_samples_ms']),
        'original_cpu_loop_warm_samples_ms':cost['original_cpu_warm_samples_ms'],
        'original_cpu_loop_warm_mean_ms':cost['original_cpu_warm_mean_ms'],
        'original_cpu_loop_warm_median_ms':cost['original_cpu_warm_median_ms'],
        'fused_median_ms':cost['fused_component_medians_ms'],
        'fused_full_mean_ms':cost['fused_full_mean_ms'],
        'fused_full_range_ms':cost['fused_full_range_ms'],
        'composed_nonoverlapped_estimate_ms':cost['composed_nonoverlapped_median_ms'],
        'composed_nonoverlapped_mean_estimate_ms':cost['composed_nonoverlapped_mean_ms'],
        'regression_vs_cpu_warm_mean_ms':cost['regression_vs_cpu_warm_mean_ms'],
        'regression_vs_cpu_warm_median_ms':cost['regression_vs_cpu_warm_median_ms'],
        'decision':cost['decision'],'cost_analysis_file':'cost-analysis.json',
    })
    if kind=='append':result['warm_original_loop_ms']=cost['original_cpu_warm_median_ms']
    else:result['estimated_regression_ms']=cost['regression_vs_cpu_warm_median_ms']
    result['files_sha256'][FILES[kind][1]]=_sha(folder/FILES[kind][1])
    result['files_sha256']['cost-analysis.json']=_sha(cost_path)
    result_path.write_text(json.dumps(result,indent=2)+'\n')
    return cost


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind',choices=sorted(FILES))
    parser.add_argument('results_dir',type=Path)
    parser.add_argument('--check',action='store_true',help='verify saved cost-analysis.json without rewriting')
    args=parser.parse_args()
    if args.check:
        actual=analyze(args.results_dir,args.kind)
        saved=json.loads((args.results_dir/'cost-analysis.json').read_text())
        if actual!=saved:raise SystemExit('saved cost analysis differs from raw evidence')
    else:actual=write_analysis(args.results_dir,args.kind)
    print(json.dumps({k:actual[k] for k in ('kind','gather_median_ms','gather_mean_ms',
                                           'fused_full_median_ms','fused_full_mean_ms',
                                           'composed_nonoverlapped_median_ms','composed_nonoverlapped_mean_ms',
                                           'regression_vs_cpu_warm_mean_ms','decision')}))


if __name__=='__main__':main()
