"""Bounded, name-blind exact-byte correspondence hypotheses. Never authorizes replacement."""
import argparse,collections,json,math,struct,time
from pathlib import Path

def discover(spec,root,seconds=60):
    started=time.monotonic();frames=spec['frames']
    if len(frames)!=2:raise ValueError('two synchronized observations required')
    gpu=[(root/f['gpu']).read_bytes() for f in frames]
    if any(len(g)>32*1024*1024 for g in gpu):raise ValueError('GPU byte budget')
    inventory=[{str(r['id']):r for r in f['cpu']} for f in frames]
    if any(len(x)>64 for x in inventory):raise ValueError('CPU region budget')
    needles=[];seen=set()
    for offset in range(0,min(4096,len(gpu[0])-11),4):
        b=gpu[0][offset:offset+12]
        if b in seen:continue
        seen.add(b);v=struct.unpack('<3f',b)
        if not all(math.isfinite(x) and abs(x)<1e8 for x in v) or max(map(abs,v))<1e-4:continue
        # Reject ubiquitous constants before scanning CPU memory.
        pos=-1;locations=[]
        for _ in range(5):
            pos=gpu[0].find(b,pos+1)
            if pos<0:break
            locations.append(pos)
        if len(locations)>4:continue
        needles.append((offset,b))
        if len(needles)==192:break
    results=[];regions_scanned=0;models_tested=0
    for identity in sorted(inventory[0].keys() & inventory[1].keys()):
        if time.monotonic()-started>seconds:raise TimeoutError('correspondence time budget')
        rows=[inventory[i][identity] for i in range(2)]
        data=[(root/r['file']).read_bytes() for r in rows]
        if any(len(x)>262144 for x in data):raise ValueError('CPU window budget')
        matches=[];regions_scanned+=1
        for go,b in needles:
            positions=[];p=-1
            for _ in range(9):
                p=data[0].find(b,p+1)
                if p<0:break
                positions.append(p)
            if len(positions)>8:continue
            matches.extend((go,p) for p in positions if p%4==0)
        matches.sort();votes=collections.Counter()
        for i,(g,c) in enumerate(matches):
            for h,d in matches[i+1:]:
                gs=h-g
                if gs>1024:break
                cs=d-c
                if gs<16 or cs<4 or cs>4096 or cs%4:continue
                phase=g%gs;intercept=c-(g//gs)*cs
                if intercept<0:continue
                votes[(gs,cs,phase,intercept)]+=1
        for (gs,cs,phase,intercept),vote in votes.most_common(256):
            if vote<5:break
            if time.monotonic()-started>seconds:raise TimeoutError('correspondence validation budget')
            models_tested+=1;indices=list(range(32))+list(range(64,96));valid=True;changed=0
            for index in indices:
                go=phase+index*gs;co=intercept+index*cs
                if any(go+12>len(gpu[k]) or co+12>len(data[k]) or gpu[k][go:go+12]!=data[k][co:co+12] for k in range(2)):
                    valid=False;break
                changed+=gpu[0][go:go+12]!=gpu[1][go:go+12]
            if valid and changed>=4:
                results.append(dict(cpu_region=identity,cpu_offset=intercept,cpu_stride=cs,gpu_offset=phase,gpu_stride=gs,width=12,validated_records_per_frame=len(indices),changed_records=changed,seed_votes=vote,status='observed_exact_affine_correspondence',replacement_allowed=False))
    # Prefer the smallest repeating stride among equally supported aliases; retain ambiguity.
    results.sort(key=lambda r:(-r['changed_records'],r['gpu_stride'],r['cpu_stride'],r['cpu_region'],r['gpu_offset']))
    for i,r in enumerate(results):
        r['subsample_of']=None
        for j,b in enumerate(results[:i]):
            if b['subsample_of'] is not None or r['cpu_region']!=b['cpu_region']:continue
            k,rem=divmod(r['gpu_stride'],b['gpu_stride'])
            shift,phase_rem=divmod(r['gpu_offset']-b['gpu_offset'],b['gpu_stride'])
            if k>1 and not rem and not phase_rem and 0<=shift<k and r['cpu_stride']==k*b['cpu_stride'] and r['cpu_offset']==b['cpu_offset']+shift*b['cpu_stride']:
                r['subsample_of']=j;break
    return dict(schema=1,regions_scanned=regions_scanned,seed_needles=len(needles),models_tested=models_tested,elapsed_seconds=time.monotonic()-started,relations=results,claims={'automatic_layout_correspondence':True,'automatic_capture_selection':False,'semantic_meaning_inferred':False,'ownership_proven':False,'automatic_offload':False})

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('manifest',type=Path);p.add_argument('output',type=Path);a=p.parse_args();r=discover(json.loads(a.manifest.read_text()),a.manifest.parent);a.output.write_text(json.dumps(r,indent=2)+'\n');print('relations',len(r['relations']),'seconds',r['elapsed_seconds'])
