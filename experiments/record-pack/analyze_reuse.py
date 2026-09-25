"""Independent raw replay of pool timing and same-frame GPU field correspondence."""
import argparse,csv,json,struct,statistics,collections,hashlib
from pathlib import Path

def analyze(root,mask_csv):
    result={'scope':'source-assisted controlled fixture; not a live ARC replacement','timing':{},'gpu_audits':[]}
    for name in ['fresh','reuse']:
        rows=list(csv.DictReader((root/name/'calls.csv').open()))
        if len(rows)>=2048:raise ValueError('Truncated counter log')
        frames=sorted({int(x['frame']) for x in rows if int(x['records'])>=1024})
        excluded=set()
        for p in (root/name).glob('audit.*.json'):
            j=json.loads(p.read_text());excluded.update([j['frame'],j['read_after_frame']])
        selected=[x for x in rows if frames[10]<=int(x['frame'])<frames[-1] and int(x['frame']) not in excluded]
        if any(int(x['pool_skip']) for x in selected):raise ValueError('Pool exhaustion')
        if any(int(x['records'])>131072 or int(x['forward']) or int(x['prepare_ns'])<=0 for x in selected):raise ValueError('Unmeasured or unsupported preparation path')
        fields=['allocation_ns','gather_ns','release_ns','record_copy_ns','prepare_ns']
        metrics={k.replace('_ns','_ms'):statistics.median(int(x[k])/1e6 for x in selected) for k in fields}
        cpu=[(int(x['loop_ns'])-int(x['flush_inside_ns']))/1e6 for x in selected]
        metrics.update(cpu_slice_ms=statistics.median(cpu),headroom_before_gpu_ms=statistics.median(c-int(x['prepare_ns'])/1e6 for c,x in zip(cpu,selected)),calls=len(selected),frames=len({x['frame'] for x in selected}),records=dict(collections.Counter(x['records'] for x in selected)),excluded_audit_frames=sorted(excluded))
        result['timing'][name]=metrics
    previous=None
    for i in range(2):
        base=root/'reuse'/f'audit.{i}'
        meta=json.loads(Path(str(base)+'.json').read_text())
        gpu=Path(str(base)+'.gpu.bin').read_bytes();expected=Path(str(base)+'.expected.bin').read_bytes()
        if len(gpu)!=meta['instances']*meta['stride'] or len(expected)!=meta['objects']*36:raise ValueError('Truncated snapshot')
        if not meta['wait_for_gpu'] or meta['read_after_frame']<=meta['frame']:raise ValueError('Missing completion ordering')
        errors=0;alpha_errors=0;alpha_test_predicate_errors=0;changed=0
        for n in range(meta['objects']):
            pos=n*meta['stride'];ep=n*36
            fade,radius,alpha,alpha_ref,lod,stencil=struct.unpack_from('<4f2I',expected,ep+12)
            flags=struct.unpack_from('<I',gpu,pos+meta['flags_offset'])[0]
            bad=(gpu[pos+meta['center_offset']:pos+meta['center_offset']+12]!=expected[ep:ep+12] or
                 gpu[pos+meta['fade_offset']:pos+meta['fade_offset']+4]!=expected[ep+12:ep+16] or
                 gpu[pos+meta['radius_offset']:pos+meta['radius_offset']+4]!=expected[ep+16:ep+20] or flags>>24!=stencil)
            errors+=bad
            ah=struct.unpack_from('<e',gpu,pos+meta['color_offset']+6)[0]
            at=struct.unpack_from('<e',gpu,pos+meta['alpha_test_offset'])[0]
            alpha_errors+=ah!=alpha;alpha_test_predicate_errors+=(at>0)!=(alpha_ref<1)
            if previous is not None:changed+=gpu[pos+meta['center_offset']:pos+meta['center_offset']+12]!=previous[ep:ep+12]
        if errors or alpha_errors or alpha_test_predicate_errors:raise ValueError('Field mismatch in tested scene')
        if i and not changed:raise ValueError('No dynamic evidence against stale-frame reuse')
        result['gpu_audits'].append(dict(meta,verified_field_errors=errors,verified_alpha_errors=alpha_errors,verified_alpha_predicate_errors=alpha_test_predicate_errors,stale_previous_centers_rejected=changed,gpu_sha256=hashlib.sha256(gpu).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest()))
        previous=expected
    rows=list(csv.DictReader(mask_csv.open()));result['mask_replay']={}
    for mode in ['mask_original','packet_prepare']:
        v=[int(x['ns'])/1e6 for x in rows if x['mode']==mode and int(x['iteration'])>=0]
        result['mask_replay'][mode]={'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v),'samples':len(v)}
    result['mask_replay']['scope']='source-equivalent CPU replay on reused D3D12 UPLOAD buffers; live-ins assumed ready; no GPU launches or measured full replacement'
    result['mask_replay']['headroom_before_gpu_ms']=result['mask_replay']['mask_original']['median_ms']-result['mask_replay']['packet_prepare']['median_ms']
    # Concrete information-loss witness, separate from the actual scene snapshots.
    a=struct.unpack('<f',struct.pack('<f',.9999))[0];h=struct.unpack('<e',struct.pack('<e',a))[0]
    result['alpha_counterexample']={'cpu_alpha':a,'gpu_half_alpha':h,'cpu_positive_transparency':1-a>0,'gpu_positive_transparency':1-h>0}
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('mask_csv',type=Path);p.add_argument('output',type=Path);a=p.parse_args();j=analyze(a.directory,a.mask_csv);a.output.write_text(json.dumps(j,indent=2)+'\n',encoding='utf-8');print(json.dumps(j['timing'],indent=2))
