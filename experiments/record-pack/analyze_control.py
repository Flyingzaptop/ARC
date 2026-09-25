"""Analyze source-assisted measurements without charging parent work to the slice."""
import argparse,csv,json,statistics,struct,hashlib,collections,math
from pathlib import Path

def analyze(directory):
    report={'scope':'source-assisted timing of automatically selected package; no GPU replacement','runs':{}}
    for run in json.loads((directory/'runs.json').read_text()):
        rows=list(csv.DictReader((directory/run['name']/'calls.csv').open()))
        if len(rows)>=2048:raise ValueError('Observer capacity reached; frequency incomplete')
        frames=sorted({int(x['frame']) for x in rows if int(x['records'])>=1024})
        chosen=[x for x in rows if frames[10]<=int(x['frame'])<frames[-1]]
        if not chosen:raise ValueError('No steady measurement')
        if any(int(x['forward']) for x in chosen):raise ValueError('Forward-light AABB contract is outside this packet experiment')
        frequencies=collections.Counter(int(x['frame']) for x in chosen)
        cpu=[(int(x['loop_ns'])-int(x['flush_inside_ns']))/1e6 for x in chosen]
        prep=[int(x['prepare_ns'])/1e6 for x in chosen]
        margin=[c-p for c,p in zip(cpu,prep)]
        value={'samples':len(chosen),'frames':len(frequencies),'calls_per_frame':dict(collections.Counter(frequencies.values())),
            'records':dict(collections.Counter(int(x['records']) for x in chosen)),'words':dict(collections.Counter(int(x['words']) for x in chosen)),
            'groups':dict(collections.Counter(int(x['groups']) for x in chosen)),
            'cpu_slice_median_ms':statistics.median(cpu),'prepare_median_ms':statistics.median(prep),
            'paired_headroom_before_gpu_median_ms':statistics.median(margin),
            'prepare_exceeds_slice_samples':sum(p>=c for p,c in zip(prep,cpu)) if run['prepare'] else None,
            'retained_flush_median_ms':statistics.median(int(x['flush_all_ns'])/1e6 for x in chosen),
            'forward_flags':dict(collections.Counter(int(x['forward']) for x in chosen)),
            'cost_scope':'elapsed original loop minus nested flush. No deliberate waits in slice; scheduling delay is not separately measured. Preparation includes gather/materialization, not transfer or GPU.'}
        if 'grid_gate' in chosen[0]:value['grid_gate_histogram']=dict(collections.Counter(int(x['grid_gate']) for x in chosen))
        packet=directory/run['name']/'calls.csv.input.bin'
        if packet.exists():
            data=packet.read_bytes();expected=(packet.parent/'calls.csv.output.bin').read_bytes();output=[];groups=[]
            f32=lambda v:struct.unpack('<f',struct.pack('<f',v))[0]
            for rec in struct.iter_unpack('<4I4f4I',data):
                mesh,identity,z,w,trans,fade,radius,alpha,lod,stencil,_,_=rec
                dist=struct.unpack('<e',struct.pack('<H',z&65535))[0]
                if not all(math.isfinite(v) for v in (dist,trans,fade,radius,alpha)) or radius<=0:raise ValueError('Unsupported exceptional numeric input')
                dither=max(trans,f32(max(0.,f32(dist-fade))/radius))
                resolved=(lod&255) if (w&255)==255 else w&255
                k=[mesh,resolved,stencil]
                if not groups or groups[-1]['key']!=k:groups.append({'key':k,'offset':len(output),'count':0,'alpha':False})
                if dither>.99:continue
                groups[-1]['alpha']|=dither>0 or alpha<1
                mask=z>>16
                while mask:
                    b=(mask&-mask).bit_length()-1;mask&=mask-1
                    output.append((identity&0xffffff)|(b<<24)|((int(f32(dither*15.))&15)<<28));groups[-1]['count']+=1
            actual=struct.pack('<'+'I'*len(output),*output)
            if actual!=expected:raise ValueError('Packet model differs from original output')
            value['packet']={'bytes':len(data),'records':len(data)//48,'output_bytes':len(actual),'groups':groups,'exact_output_match':True,'input_sha256':hashlib.sha256(data).hexdigest(),'output_sha256':hashlib.sha256(expected).hexdigest()}
        report['runs'][run['name']]=value
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=analyze(a.directory);a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print({k:(v['cpu_slice_median_ms'],v['prepare_median_ms']) for k,v in result['runs'].items()})
