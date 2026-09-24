"""Optional static PNG companion to the unsmoothed SVG reports."""
import argparse,csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=argparse.ArgumentParser();p.add_argument('raw',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
for renderer in ['wicked','cauldron']:
 fig,axes=plt.subplots(3,1,figsize=(12,9),layout='constrained');maxima=[0.,0.,0.]
 for name,color in zip(['A1','B1','B2','A2'],['#2563eb','#ea580c','#b91c1c','#0891b2']):
  with (a.raw/renderer/name/'joined-frames.csv').open() as f:rows=list(csv.DictReader(f))
  for i,(ax,key,label) in enumerate(zip(axes,['cpu_ms','gpu_ms','present_rate_hz'],['CPU processing through submission, ms','GPU graphics span, ms','Successful Present returns/s (not displayed FPS)'])):
   pts=[(float(x['time_s']),float(x[key])) for x in rows if x.get(key)];maxima[i]=max(maxima[i],max(y for x,y in pts))
   ax.plot([x for x,y in pts],[y for x,y in pts],color=color,lw=.7,alpha=.7,label=name);ax.set_ylabel(label,fontsize=9);ax.grid(alpha=.2)
 for ax,maximum in zip(axes,maxima):ax.set_ylim(0,maximum*1.05)
 axes[0].legend(ncol=4);axes[-1].set_xlabel('Seconds from measurement start');fig.suptitle(renderer+' — full ARC comparison; all samples, no smoothing')
 fig.savefig(a.output/(renderer+'-plots.png'),dpi=130);plt.close(fig)
