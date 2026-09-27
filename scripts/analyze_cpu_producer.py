"""Verify live provenance, targeted producer capture, replay and later completed upload."""
import hashlib,json,sys
from pathlib import Path
from cpu_producer_replay import run as replay
from gpu_candidate_pipeline import snapshots_from_events
def valid_scope(b,prefix,live,last_qpc):
    if hashlib.sha256(prefix).hexdigest()!=b['session_event_sha256'] or not live.startswith(prefix):return False
    events=[json.loads(x) for x in live.splitlines()]
    return not any(e['event']=='invalidate' and e['resource']==b['resource'] and e.get('generation',b['generation'])==b['generation'] and e.get('qpc',0)<=last_qpc for e in events)
def valid_stop(w,s,code):return w['armed']==w['restored'] and s['armed']==s['restored'] and not s['active_trap'] and code==0
def analyze(root):
    binding=json.loads((root/'watch/binding.json').read_text());prefix=(root/'binding/events.jsonl').read_bytes();live=(root/'live/events.jsonl').read_bytes();b=binding['binding'];r=binding['relation']
    result={'status':'unconfirmed','binding_identity':{'session_prefix_sha256':b['session_event_sha256'],'resource':b['resource'],'generation':b['generation'],'range':b['range']},'replacement_allowed':False}
    if hashlib.sha256(prefix).hexdigest()!=b['session_event_sha256'] or not live.startswith(prefix):result['status']='invalidated';result['reason']='session prefix changed';return result
    events=[json.loads(x) for x in live.splitlines()];writes=[json.loads(x) for x in (root/'watch/writes.jsonl').read_text().splitlines()];trace=[json.loads(x) for x in (root/'slice/trace.jsonl').read_text().splitlines()]
    if not writes or not trace:result['status']='insufficient_observations';return result
    if len({e['rip_after'] for e in writes})!=1:result['status']='ambiguous_producer';return result
    if any(e['target']!=binding['target'] for e in writes):result['status']='invalidated';result['reason']='wrong watched producer range';return result
    last=max(e['qpc'] for e in trace)
    if not valid_scope(b,prefix,live,last):result['status']='invalidated';result['reason']='resource invalidated during observation';return result
    watch_done=json.loads((root/'watch/done.json').read_text());slice_done=json.loads((root/'slice/done.json').read_text());native=json.loads((root/'run.json').read_text())
    clean=valid_stop(watch_done,slice_done,native['exit_code'])
    result['stop_verified']=clean;result['watch_cost']=watch_done;result['slice_cost']=slice_done
    if not clean:result['status']='unsafe_stop';return result
    model=replay(root/'slice');result['replay']=model;plan=json.loads((root/'slice/plan.json').read_text());result['writer']={k:plan[k] for k in ['writer','writer_pc','writer_rva','output_bytes','observed_tid']}
    if not model['exact'] or not model['holdout_passed']:result['status']='fragment_not_confirmed';return result
    classifications={c['classification'] for c in model['calls']};result['classifications']=sorted(classifications)
    if 'origin_unresolved_register_value' in classifications:result['status']='unclosed_dependency';result['reason']='copied live register origin not recovered';return result
    completed={e['snapshot']:e for e in events if e['event']=='completed'};submitted={e['snapshot']:e for e in events if e['event']=='snapshot_submitted'};valid_snapshots={e['snapshot'] for e in snapshots_from_events(events)};matches=[]
    for s in events:
        if s['event']!='snapshot_recorded' or s['snapshot']<3 or s['snapshot'] not in valid_snapshots or s['resource']!=b['resource'] or s['generation']!=b['generation']:continue
        c=completed.get(s['snapshot']);sub=submitted.get(s['snapshot'])
        if not c or not sub or c['queue']!=sub['queue'] or c['fence_value']!=sub['fence_value'] or c['completed_value']<sub['fence_value'] or c['completed_value']==(1<<64)-1:continue
        gpu=(root/f"live/s{s['snapshot']}.gpu.bin").read_bytes();offset=r['gpu_offset']-s['write_offset'];n=min(plan['output_memory']['size'],r['width'])
        for e in writes:
            if e['read_ok'] and e['qpc']<s.get('write_qpc',0) and offset>=0 and gpu[offset:offset+n]==bytes.fromhex(e['value'])[:n]:matches.append({'cpu_write_event':e['event'],'cpu_qpc':e['qpc'],'GPU_write_op':s['write_op'],'GPU_record_qpc':s['write_qpc'],'snapshot':s['snapshot'],'queue':c['queue'],'fence':c['fence_value'],'matched_bytes':n})
    result['later_upload_matches']=matches;result['status']='exact_fragment_with_later_upload_evidence' if matches else 'exact_fragment_upload_order_unconfirmed'
    if classifications=={'copy_only'}:result['status']='copy_only'
    result['observed_element_indices']=[(x['output_address']-binding['target'])//r['cpu_stride'] for x in model['calls']]
    result['unknowns']=['memory ownership and concurrent readers','first CPU consumer','first shader consumer','upstream production of the arithmetic input arrays','original uninstrumented fragment cost','all unobserved control paths and upper vector/exception state']
    return result
if __name__=='__main__':
    r=analyze(Path(sys.argv[1]));Path(sys.argv[2]).write_text(json.dumps(r,indent=2)+'\n');print(r['status'])
