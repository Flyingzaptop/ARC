"""Reproduce bounded CPU-only retention cost against a pinned ARC commit."""
import argparse,json,pathlib,re,shutil,statistics,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
def run(cmd,timeout=120):
 p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=timeout)
 if p.returncode:raise RuntimeError(p.stdout+'\n'+p.stderr)
 return p
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--old',default='324081d');p.add_argument('--out',type=pathlib.Path,default=ROOT/'build/arc2-cost-probe');p.add_argument('--cmake',default=shutil.which('cmake') or 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe');a=p.parse_args()
 if not re.fullmatch('[0-9a-fA-F]{7,40}',a.old):raise ValueError('pinned hexadecimal commit required')
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 for name in ['include/arc/arc2/ir.hpp','include/arc/arc2/runtime.hpp','src/arc2/runtime.cpp']:
  target=out/'old'/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(subprocess.check_output(['git','-C',str(ROOT),'show',a.old+':'+name]))
 source=(ROOT/'tests/arc2_ir_cost.cpp').as_posix();current=(ROOT/'src/arc2/runtime.cpp').as_posix();include=(ROOT/'include').as_posix()
 (out/'CMakeLists.txt').write_text(f'cmake_minimum_required(VERSION 3.24)\nproject(ARC2Cost LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 23)\nadd_executable(old_cost "{source}" old/src/arc2/runtime.cpp)\ntarget_include_directories(old_cost PRIVATE old/include)\nadd_executable(new_cost "{source}" "{current}")\ntarget_include_directories(new_cost PRIVATE "{include}")\n')
 run([a.cmake,'-S',out,'-B',out/'build','-A','x64']);run([a.cmake,'--build',out/'build','--config','Release','-j','2'])
 result={'old_commit':a.old,'scope':'CPU-only IR history cost; not a frame-time improvement','samples':[]}
 for mode in ['old','new','new','old','old','new']:
  exe=out/'build/Release'/f'{mode}_cost.exe';q=run([exe],30);result['samples'].append({'mode':mode,'command':[str(exe)],'stdout':q.stdout,'metrics':json.loads(q.stdout)})
 for mode in ['old','new']:result[mode+'_median_ms']=statistics.median(x['metrics']['saturated_mean_ms'] for x in result['samples'] if x['mode']==mode)
 result['saturated_speed_ratio']=result['old_median_ms']/result['new_median_ms'];(out/'comparison.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k!='samples'},indent=2))
