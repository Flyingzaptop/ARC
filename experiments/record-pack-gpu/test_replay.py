"""Targeted native GPU oracle tests. No frame-performance claim from these inputs."""
import json,random,struct,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
results=[]
for n in [1,255,256,257,65536]:
    rng=random.Random(n);data=bytearray()
    for i in range(n):
        group=i//max(1,(n+63)//64)
        distance=struct.unpack('<H',struct.pack('<e',rng.choice([0.,1.,2.,3.])))[0]
        mask=rng.choice([0,1,3,0x8080,65535])
        data+=struct.pack('<4I4f4I',group,i,distance|(mask<<16),255,rng.choice([0.,.2,.5,1.]),1.,10.,rng.choice([1.,.9]),group%4,group%3,0,0)
    p=out/f'{n}.bin';p.write_bytes(data)
    r=subprocess.run([str(root/'build/Release/arc-packet-gpu.exe'),str(p),str(root/'experiments/record-pack-gpu/pack.hlsl'),str(out/f'{n}.csv')],capture_output=True,timeout=30)
    results.append(dict(records=n,code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
    if r.returncode:raise RuntimeError(results[-1])
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print('5 GPU oracle cases passed')
