"""Bounded owned native lab runner; preserves failures and exact command lines."""
import argparse,subprocess,os,json,pathlib
p=argparse.ArgumentParser();p.add_argument('output',type=pathlib.Path);p.add_argument('--mode',choices=['native','legacy','passthrough','observe','optimize'],required=True);p.add_argument('--name',required=True);p.add_argument('--frames',default=40,type=int);p.add_argument('--clears',default=64,type=int);p.add_argument('--debug',action='store_true');p.add_argument('--toggle',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);env=os.environ.copy();env.pop('ARC2_MODE',None);env.pop('ARC1_DLL',None)
if a.mode=='legacy':env['ARC1_DLL']=str(root/'build/Release/arc-dx12-probe.dll')
if a.mode not in ['native','legacy']:env.update(ARC2_MODE=a.mode,ARC2_DLL=str(root/'build/Release/arc2-frontend.dll'),ARC2_OUTPUT=str(out/(a.name+'.ir.json')))
cmd=[str(root/'build/Release/arc2-native-lab.exe'),str(out/a.name),str(a.clears),str(a.frames)]+(['debug' if a.debug else 'nodebug','toggle'] if a.toggle else (['debug'] if a.debug else []))
try:
 r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=60);data=dict(mode=a.mode,command=cmd,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr)
except subprocess.TimeoutExpired as e:data=dict(mode=a.mode,command=cmd,timeout=60,stdout=str(e.stdout),stderr=str(e.stderr))
(out/(a.name+'.json')).write_text(json.dumps(data,indent=2));print(json.dumps(data));raise SystemExit(data.get('returncode',1))
