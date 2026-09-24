import argparse,hashlib,json,zipfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
m=json.loads((a.source/'manifest.json').read_text());archive=a.output/'raw.zip'
with archive.open('wb') as dest:
 for part in m['parts']:
  path=(a.source/part['file']).resolve()
  if not path.is_relative_to(a.source.resolve()):raise ValueError('Unsafe part path')
  h=hashlib.sha256()
  with path.open('rb') as src:
   while block:=src.read(1024*1024):h.update(block);dest.write(block)
  if h.hexdigest()!=part['sha256']:raise ValueError('Part hash mismatch')
with archive.open('rb') as f:
 if hashlib.file_digest(f,'sha256').hexdigest()!=m['archive_sha256']:raise ValueError('Archive hash mismatch')
with zipfile.ZipFile(archive) as z:
 for item in z.infolist():
  target=(a.output/item.filename).resolve()
  if not target.is_relative_to(a.output.resolve()):raise ValueError('Unsafe archive path')
  z.extract(item,a.output)
for name,entry in m['members'].items():
 with (a.output/name).open('rb') as f:
  if hashlib.file_digest(f,'sha256').hexdigest()!=entry['sha256']:raise ValueError('Member hash mismatch: '+name)
print('Verified and unpacked',len(m['members']),'files')
