"""Plot all frames using non-overlapping one-second reporting bins."""
import argparse,json,math,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args();root=a.output
summary=json.loads((root/'summary.json').read_text());series=[]
for run in summary:
 rows=[json.loads(x) for x in (root/run['label']/'frames.jsonl').read_text().splitlines()]
 tail=[r['frame_ms'] for r in rows if r['measurement_elapsed_ms']>=50000]
 run['last_10s_mean_fps']=1000*len(tail)/sum(tail)
 run['gpu_span_mean_ms']=statistics.mean(r['gpu_span_ms'] for r in rows)
 run['gpu_pass_mean_ms']={k:statistics.mean(r['gpu_ms'].get(k,0) for r in rows) for k in ['Deferred Lighting','RasterShadow','GBuffer','Brixelizer Update','Brixelizer GI Update']}
 buckets={}
 for row in rows:
  index=min(59,int(row['measurement_elapsed_ms']/1000));bucket=buckets.setdefault(index,[0,0]);bucket[0]+=1;bucket[1]+=row['frame_ms']
 series.append(([i+.5 for i in sorted(buckets)],[1000*buckets[i][0]/buckets[i][1] for i in sorted(buckets)]))
(root/'summary.json').write_text(json.dumps(summary,indent=2))
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,'axes.spines.right':False})
limit=math.ceil(max(max(v) for _,v in series)/10)*10+10
fig,axes=plt.subplots(4,2,figsize=(15,13),sharex=True,sharey=True);axes=axes.flat
for ax,run,(times,fps) in zip(axes,summary,series):
 ax.plot(times,fps,color='#147d92',linewidth=1.8,label='Measured FPS, 1 s bins');ax.fill_between(times,fps,alpha=.08,color='#147d92')
 target=run['target_fps'];title='Baseline: no ARC' if target is None else f"ARC x{run['multiplier']:g} | target {target:.1f} FPS"
 ax.set_title(title,loc='left',fontweight='bold');ax.set_xlim(0,60);ax.set_ylim(0,limit);ax.grid(alpha=.2)
 if target is None:ax.axhline(run['median_fps'],ls='--',color='#616161',label=f"Baseline median {run['median_fps']:.2f}")
 elif target<=limit:ax.axhline(target,ls='--',color='#c75e22',label='Target')
 else:ax.text(.97,.93,f'Target {target:.1f} FPS is above chart range',transform=ax.transAxes,ha='right',color='#ad4e16',fontsize=9)
 ax.text(.02,.08,f"Mean {run['mean_fps']:.2f} | Median {run['median_fps']:.2f} | 1% low {run['one_percent_low_fps']:.2f}",transform=ax.transAxes,fontsize=9)
 ax.set_ylabel('FPS');ax.set_xlabel('Measurement time (s)')
for unused in list(axes)[len(summary):]:unused.axis('off')
axes[-1].axis('off');axes[-1].text(0,.9,'Same native resolution and camera route.\n60 seconds per run; adaptation costs included.\nAll frames retained, including long frames.\n\nCurves show FPS in 1-second bins.\nTargets use median per-frame baseline FPS.\nThe route is frame-driven: equal wall times\ndo not imply identical camera positions.\n\nOne run per target, not repeatability evidence.\nNo automatic image quality checks.',va='top',linespacing=1.7)
fig.suptitle('ARC target-FPS sweep — dynamic Cauldron',fontsize=18,fontweight='bold');fig.tight_layout(rect=[0,0,1,.96]);fig.savefig(root/'fps-sweep.png',dpi=150);plt.close(fig)
fig,ax=plt.subplots(figsize=(12,4));times,fps=series[0];ax.plot(times,fps,color='#343b45');ax.axhline(summary[0]['median_fps'],ls='--',color='#c75e22',label=f"Median {summary[0]['median_fps']:.2f} FPS");ax.set(xlim=(0,60),xlabel='Measurement time (s)',ylabel='FPS',title='Cauldron without ARC — 60 seconds, 1-second FPS bins');ax.grid(alpha=.2);ax.legend();fig.tight_layout();fig.savefig(root/'baseline-fps.png',dpi=150);plt.close(fig)
fig,ax=plt.subplots(figsize=(11,5));labels=[f"x{r['multiplier']:g}" for r in summary[1:]];values=[100*r['mean_fps']/r['target_fps'] for r in summary[1:]];bars=ax.bar(labels,values,color='#147d92');ax.axhline(100,ls='--',color='#c75e22');ax.bar_label(bars,fmt='%.1f%%');ax.set(ylabel='Mean FPS / target FPS (%)',title='Target attainment — full measured window',ylim=(0,max(110,max(values)*1.15)));ax.grid(axis='y',alpha=.2);fig.tight_layout();fig.savefig(root/'target-attainment.png',dpi=150);plt.close(fig)
lines=['# Cauldron target-FPS sweep','',f"Baseline median: **{summary[0]['median_fps']:.3f} FPS**. Targets are calculated from this value.",'','| Mode | Target FPS | Mean FPS | Median FPS | Last 10s FPS | 1% low | Time at/above target | p99 ms |','|---|---:|---:|---:|---:|---:|---:|---:|']
for r in summary:
 target='—' if r['target_fps'] is None else f"{r['target_fps']:.2f}";attain='—' if r['target_fps'] is None else f"{r['time_at_or_above_target_percent']:.1f}%"
 lines.append(f"| {r['label']} | {target} | {r['mean_fps']:.2f} | {r['median_fps']:.2f} | {r['last_10s_mean_fps']:.2f} | {r['one_percent_low_fps']:.2f} | {attain} | {r['p99_frame_ms']:.2f} |")
lines+=['','60-second measurement windows after the same 120-frame host warmup. No separate controller initialization is excluded. Each ARC run starts from an identical copied shader-cache seed. Camera route/settings are identical; camera animation is frame-driven, so different FPS traverse different numbers of cycles. One run per goal; not a repeated causal benchmark.','', '## Hardware and focus','']
for r in summary:lines.append(f"- {r['label']}: power-limit readings {r['power_limit_w']} W; temperature {r['temperature_min_max_c']} C; graphics clocks {r['graphics_clock_min_max_mhz']} MHz; foreground fraction {r['foreground_fraction']:.3f}.")
lines+=['','## GPU pass costs (host measurements, milliseconds)','', '| Mode | GPU span | Lighting | Shadows | GBuffer | Brixelizer update | GI update |','|---|---:|---:|---:|---:|---:|---:|']
for r in summary:
 costs=r['gpu_pass_mean_ms'];lines.append('| '+r['label']+' | '+f"{r['gpu_span_mean_ms']:.2f}"+' | '+' | '.join(f'{v:.2f}' for v in costs.values())+' |')
lines+=['','Raw frames, per-frame fps.csv, manifests, GPU sensor logs and controller decisions remain in each run folder. No image quality assertion is made.','']
(root/'REPORT.md').write_text('\n'.join(lines),encoding='utf-8')
print(root/'fps-sweep.png')
