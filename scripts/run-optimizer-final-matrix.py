"""Frozen five-arm matrix. UI focus is managed through Computer Use, not this script."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('sdk',type=Path);p.add_argument('output',type=Path)
p.add_argument('--dll',required=True,type=Path);p.add_argument('--compiler',required=True,type=Path)
p.add_argument('--previous',required=True,type=Path,help='Frozen previous package, including its own critic/runtime')
p.add_argument('--quality-python',type=Path,default=Path(sys.executable));p.add_argument('--rounds',type=int,choices=[1,2,3],default=3)
p.add_argument('--visible',action='store_true');p.add_argument('--route',choices=['primary','holdout'],default='primary')
a=p.parse_args();root=a.output.resolve();root.mkdir(parents=True,exist_ok=False)
runner=Path(__file__).with_name('run-cauldron-benchmark.py');previous=a.previous.resolve()
old_python=previous/'runtime/python/python.exe';old_critic=previous/'scripts/optimizer-live-quality.py'
files=[a.dll.resolve(),a.dll.resolve().parent/'arc-shader-tool.exe',a.dll.resolve().parent/'arc-quality-metrics.dll',a.sdk.resolve()/'bin/FFX_BRIXELIZER_GI_DX12.exe',previous/'arc-dx12-probe.dll',previous/'arc-shader-tool.exe',old_python,old_critic,Path(__file__).with_name('optimizer-live-quality.py'),Path(__file__).with_name('optimizer_quality_metrics.py')]
def hashes():
    return {str(f):hashlib.file_digest(f.open('rb'),'sha256').hexdigest() for f in files}
frozen=hashes();orders=[['O','N','V','B','A'],['A','B','V','N','O'],['B','O','A','V','N']]
manifest=dict(binary_and_critic_hashes=frozen,orders=orders[:a.rounds],initialization_after_seconds=100,initialization_limit_seconds=120,measurement_seconds=60,process_limit_seconds=210,requested_power_w=30,route=a.route,visible=a.visible,presentation="borderless")
(root/'matrix-manifest.json').write_text(json.dumps(manifest,indent=2));completed=[]
def status(value):
    temporary=root/'matrix-status.tmp';temporary.write_text(json.dumps(value,indent=2));temporary.replace(root/'matrix-status.json')
for series,order in enumerate(orders[:a.rounds],1):
    for mode in order:
        if hashes()!=frozen:raise RuntimeError('Frozen input changed during matrix')
        case=root/f'{series:02d}-{mode}'
        command=[sys.executable,str(runner),str(a.sdk.resolve()),str(case),'--initialization-seconds','100','--measurement-seconds','60','--route',a.route,'--borderless']
        if a.visible:command+=['--visible']
        if mode!='O':
            dll=previous/'arc-dx12-probe.dll' if mode=='V' else a.dll.resolve()
            command+=['--dll',str(dll),'--compiler',str(a.compiler.resolve()),'--mode','compute-off','--cache',str(root/('previous-cache' if mode=='V' else 'new-cache'))]
        if mode in ('V','B','A'):
            command+=['--auto-target','144','--quality-profile','aggressive' if mode=='A' else 'balanced']
            if mode=='V':command+=['--shader-worker',str(previous/'arc-shader-tool.exe'),'--critic',str(old_critic),'--quality-python',str(old_python)]
            else:command+=['--quality-python',str(a.quality_python)]
        status(dict(phase='running',case=str(case),series=series,mode=mode,completed=completed,command=command))
        result=subprocess.run(command)
        if result.returncode:
            status(dict(phase='failed',case=str(case),exit_code=result.returncode,completed=completed));raise SystemExit(result.returncode)
        details=json.loads((case/'manifest.json').read_text());completed.append(dict(case=str(case),comparable=details.get('measurement_phase',{}).get('comparable',False),mode=mode,series=series))
status(dict(phase='complete',completed=completed,binary_and_critic_hashes=frozen))
print(json.dumps(dict(matrix=str(root),completed=len(completed))),flush=True)
