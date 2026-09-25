"""Automatic bounded candidate -> completed-write snapshot -> discovery -> new observation.
Oracle metadata is never read here. Confirmation is observational, not offload admission.
"""
import hashlib,json,sys
from pathlib import Path
from cpu_gpu_correspondence import discover

def snapshots_from_events(events):
    records={};submits={};complete={};invalid=set();selected={};created={}
    for e in events:
        kind=e['event']
        if kind=='create':created[e['resource']]=e
        elif kind=='selected':selected[e['resource']]=e
        elif kind=='snapshot_recorded':records[e['snapshot']]=e
        elif kind=='snapshot_submitted':submits[e['snapshot']]=e
        elif kind=='completed':complete[e['snapshot']]=e
        elif kind=='invalid_snapshot':invalid.add(e['snapshot'])
    result=[]
    for key,r in records.items():
        s=submits.get(key);c=complete.get(key)
        if key in invalid or not s or not c:continue
        if r['resource'] not in selected or r['resource'] not in created:continue
        if r['write_offset']<0 or r['snapshot_bytes']<=0 or r['snapshot_bytes']>min(r['write_bytes'],128*1024):continue
        if r['write_offset']+r['write_bytes']>created[r['resource']]['bytes']:continue
        if not(selected[r['resource']]['event_id']<r['event_id']<s['event_id']<c['event_id']):continue
        if c['completed_value']==(1<<64)-1:continue
        if s['recording']!=r['recording'] or s['queue']!=c['queue'] or s['fence_value']!=c['fence_value'] or c['completed_value']<s['fence_value']:continue
        if any(r[k]!=c[k] for k in ['resource','generation','write_op']):continue
        result.append({**r,'completion_event':c['event_id'],'queue':s['queue'],'fence_value':s['fence_value']})
    return sorted(result,key=lambda r:r['write_op'])

def confirm(relation,cpu_rows,gpu,root,previous):
    item=next((r for r in cpu_rows if str(r['address'])==relation['cpu_region']),None)
    if item is None:return 'cpu_placement_changed'
    cpu=(root/item['file']).read_bytes();changed=0
    for i in list(range(32))+list(range(64,96)):
        a=relation['cpu_offset']+i*relation['cpu_stride'];b=relation['gpu_offset']+i*relation['gpu_stride'];n=relation['width']
        if a+n>len(cpu) or b+n>len(gpu) or cpu[a:a+n]!=gpu[b:b+n]:return 'holdout_mismatch'
        changed+=gpu[b:b+n]!=previous[b:b+n]
    return 'confirmed_on_new_write' if changed>=4 else 'inconclusive_unchanged'

def analyze(root):
    trace=(root/'events.jsonl').read_bytes();session=hashlib.sha256(trace).hexdigest()
    events=[json.loads(x) for x in trace.decode().splitlines()]
    if any(a['event_id']>=b['event_id'] for a,b in zip(events,events[1:])):raise ValueError('non-monotonic or mixed event sessions')
    valid=snapshots_from_events(events);groups={}
    for r in valid:groups.setdefault((r['resource'],r['generation'],r['write_offset'],r['snapshot_bytes']),[]).append(r)
    results=[]
    for key,observations in groups.items():
        entry={'session_event_sha256':session,'resource':key[0],'generation':key[1],'range':[key[2],key[3]],'observations':observations,'replacement_allowed':False}
        invalidations=[e for e in events if e['event']=='invalidate' and e['resource']==key[0] and e.get('generation',key[1])==key[1] and e['event_id']>observations[0]['event_id']]
        if len(observations)<3:
            entry['status']='invalidated' if invalidations else 'insufficient_observations';entry['invalidations']=invalidations;results.append(entry);continue
        observations=observations[:3]
        if any(e['event_id']<=observations[2]['completion_event'] for e in invalidations):
            entry['status']='invalidated_before_confirmation';entry['invalidations']=invalidations;results.append(entry);continue
        windows=[];frames=[]
        for o in observations:
            m=json.loads((root/f"s{o['snapshot']}.windows.json").read_text())
            if m['frame']!=o['write_op']:raise ValueError('CPU observation not tied to write')
            if (root/f"s{o['snapshot']}.gpu.bin").stat().st_size!=o['snapshot_bytes']:raise ValueError('GPU snapshot range size')
            windows.append(m['regions']);frames.append({'gpu':f"s{o['snapshot']}.gpu.bin",'cpu':[{'id':str(r['address']),'file':r['file']} for r in m['regions']]})
        learned=discover({'frames':frames[:2]},root,seconds=60)
        third=(root/frames[2]['gpu']).read_bytes();previous=(root/frames[1]['gpu']).read_bytes()
        for r in learned['relations']:r['confirmation']=confirm(r,windows[2],third,root,previous)
        entry['discovery']=learned;entry['confirmation_snapshot']=observations[2]['snapshot'];entry['status']='confirmed' if any(r['subsample_of'] is None and r['confirmation']=='confirmed_on_new_write' for r in learned['relations']) else 'hypotheses_only';entry['invalidations']=invalidations;entry['live_binding_valid']=entry['status']=='confirmed' and not invalidations
        results.append(entry)
    return {'schema':1,'session_event_sha256':session,'selected_candidates':[e for e in events if e['event']=='selected'],'validated_snapshot_count':len(valid),'bindings':results,'automatic_offload':False,'semantic_meaning_inferred':False}
if __name__=='__main__':
    root=Path(sys.argv[1]);result=analyze(root);Path(sys.argv[2]).write_text(json.dumps(result,indent=2)+'\n');print([(x['resource'],x['status']) for x in result['bindings']])
