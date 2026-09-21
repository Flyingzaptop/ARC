"""Independent native-resolution comparison at identical absolute scene frames."""
import os
for key in ('OPENBLAS_NUM_THREADS','OMP_NUM_THREADS','MKL_NUM_THREADS'):
    os.environ[key]='1'
import argparse,json,runpy,sys
from pathlib import Path
import numpy as np
from PIL import Image
root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'scripts'),str(root/'build/quality-worker')]
live=runpy.run_path(str(root/'scripts/optimizer-live-quality.py'))
from optimizer_quality_metrics import compare_striped,accepts,PROFILES

def load(path):return np.asarray(Image.open(path).convert('RGB'),dtype=np.float64)/255

def main():
    parser=argparse.ArgumentParser();parser.add_argument('reference',type=Path);parser.add_argument('candidate',type=Path)
    parser.add_argument('--profile',choices=list(PROFILES),default='balanced');parser.add_argument('--repeat-reference',action='store_true')
    args=parser.parse_args();a=args.reference.resolve();b=args.candidate.resolve()
    ma=json.loads((a/'manifest.json').read_text());mb=json.loads((b/'manifest.json').read_text())
    if ma['host_sha256']!=mb['host_sha256'] or ma['route']!=mb['route']:raise ValueError('Different renderer or trajectory')
    if not ma.get('oracle_poses') or not mb.get('oracle_poses'):raise ValueError('Fixed-pose oracle required')
    frames_a={int(p.stem.split('-')[-1]):p for p in a.glob('oracle-*.png')}
    frames_b={int(p.stem.split('-')[-1]):p for p in b.glob('oracle-*.png')}
    common=sorted(frames_a.keys()&frames_b.keys());checks=[];temporal=[];previous=None
    for frame in common:
        reference,candidate=load(frames_a[frame]),load(frames_b[frame])
        if reference.shape!=(1080,1920,3) or candidate.shape!=reference.shape:raise ValueError('Non-native oracle image')
        metrics=compare_striped(reference,candidate,filter_fn=live['fast_gaussian'])
        passed=accepts(metrics,args.profile)
        if args.repeat_reference:passed=(metrics['ssim_gaussian_luma']>=.999 and metrics['mean_linear_rgb_error']<=.001 and metrics['p99_tile_linear_rgb_error']<=.004 and metrics['worst_tile_linear_rgb_error']<=.015)
        annotation=None;state=b/f'oracle-{frame}.arc.json'
        if state.exists():
            try:annotation=json.loads(state.read_text()).get('automatic_session')
            except (ValueError,OSError):annotation={'unavailable':True}
        checks.append(dict(scene_frame=frame,pose=frame%600,reference=str(frames_a[frame]),candidate=str(frames_b[frame]),metrics=metrics,passed=passed,post_present_control_annotation=annotation,annotation_is_gpu_execution_proof=False))
        residual=(candidate**2.2-reference**2.2).astype(np.float32)
        if previous and frame==previous[0]+1:
            evidence=(dict(p99_tile_error=0.,mean_error=0.,matched_reference=True,alignment_coverage=1.,flow_source='exact_zero_error_fields_motion_independent') if not np.any(previous[2]) and not np.any(residual) else live['temporal'](previous[1],reference,previous[2],residual))
            evidence.update(first_frame=previous[0],second_frame=frame)
            evidence['passed']=evidence['matched_reference'] and evidence['p99_tile_error']<=PROFILES[args.profile]['temporal']
            temporal.append(evidence)
        previous=(frame,reference,residual)
    complete=len(common)>=25 and frames_a.keys()==frames_b.keys()
    result=dict(schema=1,quality_profile=args.profile,thresholds=PROFILES[args.profile],same_absolute_scene_frames=True,complete=complete,matched_frames=len(common),repeat_reference=args.repeat_reference,all_image_checks_passed=complete and all(c['passed'] for c in checks),all_temporal_checks_passed=bool(temporal) and all(c['passed'] for c in temporal),checks=checks,temporal=temporal)
    worst={}
    for key in ('ssim_gaussian_luma','mean_linear_rgb_error','p99_tile_linear_rgb_error','worst_tile_linear_rgb_error'):
        if checks:worst[key]=(min if key=='ssim_gaussian_luma' else max)(checks,key=lambda c:c['metrics'][key])['scene_frame']
    result['worst_frames']=worst
    output=b/('reference-repeat-quality.json' if args.repeat_reference else 'independent-quality.json')
    output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({k:result[k] for k in ('matched_frames','complete','all_image_checks_passed','all_temporal_checks_passed','worst_frames')}))

if __name__=='__main__':main()
