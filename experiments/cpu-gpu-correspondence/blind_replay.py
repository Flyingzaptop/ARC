import importlib.util,json,os,sys,uuid
from pathlib import Path
root=Path(__file__).resolve().parents[2];s=importlib.util.spec_from_file_location('learner',root/'scripts/cpu_gpu_correspondence.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
source=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=False);spec=json.loads((source/'input.json').read_text());rename={};index=0
for f in spec['frames']:
    old=f['gpu'];new=f'blob-{index}.bin';index+=1;os.link(source/old,out/new);f['gpu']=new
    for r in f['cpu']:
        original=r['id'];rename.setdefault(original,str(uuid.uuid5(uuid.NAMESPACE_OID,'opaque-'+original)));r['id']=rename[original];old=r['file'];new=f'blob-{index}.bin';index+=1;os.link(source/old,out/new);r['file']=new
(out/'input.json').write_text(json.dumps(spec,indent=2));a=m.discover(json.loads((source/'input.json').read_text()),source);b=m.discover(spec,out)
reverse={v:k for k,v in rename.items()}
for r in b['relations']:r['cpu_region']=reverse[r['cpu_region']]
assert a['relations']==b['relations']
(out/'validation.json').write_text(json.dumps({'renamed_files':index,'opaque_identifiers':len(rename),'identical_relations':True},indent=2)+'\n');print('real raw-byte replay invariant to filenames and region identifiers')
