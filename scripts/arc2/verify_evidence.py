"""Inventory reproducible ARC2 evidence, including archived member hashes."""
import argparse,hashlib,json,pathlib,subprocess,zipfile

def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
def inventory(root):
 rows=[]
 for path in sorted(root.rglob('*')):
  if not path.is_file() or path==root/'manifest.json':continue
  row=dict(path=path.relative_to(root).as_posix(),bytes=path.stat().st_size,sha256=digest(path))
  if path.suffix=='.zip':
   with zipfile.ZipFile(path) as archive:
    members=[]
    for info in archive.infolist():
     if info.is_dir():continue
     h=hashlib.sha256()
     with archive.open(info) as stream:
      for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
     members.append(dict(path=info.filename,bytes=info.file_size,sha256=h.hexdigest()))
    row['members']=members
  rows.append(row)
 return dict(schema='arc2-evidence-manifest-v1',files=rows)

def verify_git(root, expected, ref):
 repo=pathlib.Path(subprocess.check_output(['git','rev-parse','--show-toplevel'],text=True).strip())
 prefix=root.resolve().relative_to(repo.resolve()).as_posix()
 process=subprocess.Popen(['git','cat-file','--batch'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
 try:
  for row in expected['files']:
   query=f"{ref}:{prefix}/{row['path']}\n"
   process.stdin.write(query.encode('utf-8'));process.stdin.flush()
   header=process.stdout.readline().split()
   if len(header)!=3 or header[1]!=b'blob':raise ValueError('missing Git evidence: '+row['path'])
   remaining=int(header[2]); h=hashlib.sha256()
   while remaining:
    chunk=process.stdout.read(min(remaining,1024*1024))
    if not chunk:raise ValueError('truncated Git blob')
    h.update(chunk);remaining-=len(chunk)
   if process.stdout.read(1)!=b'\n':raise ValueError('invalid Git batch response')
   if int(header[2])!=row['bytes'] or h.hexdigest()!=row['sha256']:
    raise ValueError('Git changed evidence bytes: '+row['path'])
  process.stdin.close()
  if process.wait()!=0:raise ValueError('Git verification failed')
 finally:
  if process.poll() is None:process.kill();process.wait()

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('--check',action='store_true');p.add_argument('--git-ref');a=p.parse_args();actual=inventory(a.root);target=a.root/'manifest.json'
 if a.check:
  expected=json.loads(target.read_text(encoding='utf-8'))
  if actual!=expected:raise SystemExit('evidence changed')
 else:target.write_text(json.dumps(actual,indent=2),encoding='utf-8')
 if a.git_ref:verify_git(a.root,actual,a.git_ref)
 print('verified' if a.check else 'inventoried',len(actual['files']),'files')

