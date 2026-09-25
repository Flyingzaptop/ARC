"""Bounded isolated correctness/economics run; never launches or attaches a game."""
import argparse,csv,hashlib,json,os,random,statistics,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_large_sort import exact_sort
from cpu_sort_contract import key

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('study',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    candidate=a.study/'final-training/candidate-00';closure=a.study/'logical-plan/candidate-00/checked-functions.bin'
    exe=ROOT/'build/Release/arc-exact-sort-gpu.exe';shader=Path(__file__).with_name('exact_sort.hlsl')
    bits=json.loads((candidate/'key-hypotheses.json').read_text())['hypotheses'][0]['projection']
    env={k:v for k,v in os.environ.items() if k!='ARC_SORT_DEBUG'}
    hardware=None
    try:
        hardware=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,name,driver_version,power.draw,power.limit,temperature.gpu,clocks.gr,clocks.mem,utilization.gpu','--format=csv','-lms','100','-f',str(a.output/'hardware.csv')],creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    except OSError:pass
    runs=[]
    def run(name,inp,expected,ideal,reps,debug):
        directory=a.output/name;directory.mkdir()
        runenv=env.copy()
        if debug:runenv['ARC_SORT_DEBUG']='1'
        command=[str(exe),str(inp),str(expected),str(closure),str(shader),str(directory/'timings.csv'),str(reps),str(ideal)]
        start=time.time();result=subprocess.run(command,env=runenv,capture_output=True,timeout=45)
        (directory/'stdout.txt').write_bytes(result.stdout);(directory/'stderr.txt').write_bytes(result.stderr)
        record={'name':name,'start_unix':start,'elapsed_seconds':time.time()-start,'command':command,'debug_layer':debug,'returncode':result.returncode,'input_sha256':sha(inp),'expected_sha256':sha(expected)}
        runs.append(record);(a.output/'runs.json').write_text(json.dumps(runs,indent=2)+'\n',encoding='utf-8')
        if result.returncode:raise RuntimeError(f'{name}: {result.stderr.decode(errors="replace")}')
        print(name,'passed',flush=True)
    try:
        # Full saved array checked under debug layer; final speed comparisons omit it.
        inp=candidate/'input-span.bin';expected=candidate/'output-span.bin';n=inp.stat().st_size//16
        run('real-validation',inp,expected,n,1,True)
        generated=a.output/'generated';generated.mkdir();rng=random.Random(250925)
        for name,size,budget in [('ties',65344,65344),('unsigned-random',4096,4096),('forced-heap',4096,0)]:
            records=[]
            for i in range(size):
                k=(i%2) if name=='ties' else rng.getrandbits(64);v=i<<32
                for pos,b in enumerate(bits):v=(v&~(1<<b))|(((k>>pos)&1)<<b)
                records.append(v.to_bytes(16,'little'))
            data=b''.join(records);reference,_=exact_sort(data,bits,budget)
            ip=generated/(name+'.input.bin');ep=generated/(name+'.expected.bin');ip.write_bytes(data);ep.write_bytes(reference)
            run(name,ip,ep,budget,1,True)
        run('real-series-1',inp,expected,n,8,False)
        run('real-series-2',inp,expected,n,8,False)
    finally:
        if hardware and hardware.poll() is None:hardware.terminate();hardware.wait(timeout=5)
    series={}
    for runinfo in runs:
        rows=list(csv.DictReader((a.output/runinfo['name']/'timings.csv').open()))
        if any(int(r['mismatch_records']) or int(r['gpu_error']) or int(r['pending_tasks']) for r in rows):raise RuntimeError('Result check')
        stats={}
        for mode in ['cpu','gpu']:
            selected=[r for r in rows if r['mode']==mode and r['warmup']=='0']
            fields=['full_ms'] if mode=='cpu' else ['prep_ms','record_ms','submit_ms','wait_ms','consume_ms','full_ms','gpu_upload_ms','gpu_init_ms','gpu_sort_ms','gpu_return_ms','gpu_root_ms']
            stats[mode]={f:{'median':statistics.median(float(r[f]) for r in selected),'min':min(float(r[f]) for r in selected),'max':max(float(r[f]) for r in selected)} for f in fields}
        stats['gpu_over_cpu_full_ratio']=stats['gpu']['full_ms']['median']/stats['cpu']['full_ms']['median'];series[runinfo['name']]=stats
    report={'schema':1,'exe_sha256':sha(exe),'shader_sha256':sha(shader),'closure_sha256':sha(closure),'series':series,
            'scope':'Isolated exact-order operation; no automatic transfer, no game speedup, no renderer contention measured. Timings exclude validation memcmp for both modes; CPU input reset excluded, GPU upload and CPU-ready copy included.',
            'decision':'Stop ownership development for this standalone sorting boundary if full GPU path loses. Consider measured wider GPU-consumer chain separately.'}
    (a.output/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({name:series[name]['gpu_over_cpu_full_ratio'] for name in series if 'series' in name}))
if __name__=='__main__':main()
