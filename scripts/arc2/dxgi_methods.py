"""Generate mechanical DXGI forwarding from the installed Windows SDK headers."""
import re,pathlib
sdk=pathlib.Path('C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared')
text='\n'.join(p.read_text(errors='replace') for p in sorted(sdk.glob('dxgi*.h')))
def methods(name):
 m=re.search(r'\b'+name+r'\s*:\s*public\s+(\w+)\s*\{(.*?)\n\s*\};',text,re.S)
 if not m: raise ValueError(name)
 parent,body=m.groups();out=[] if parent=='IUnknown' else methods(parent)
 for typ,n,args in re.findall(r'virtual\s+([\w *]+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0;',body,re.S):
  args=re.sub(r'/\*.*?\*/','',args,flags=re.S)
  args=re.sub(r'\b_[A-Za-z0-9_]+_\((?:[^()]|\([^()]*\))*\)','',args)
  args=re.sub(r'\b_[A-Za-z0-9_]+_\b','',args)
  args=' '.join(args.split());params=[] if args=='void' else args.split(',')
  names=[re.search(r'(\w+)\s*(?:\[[^\]]*\])?$',p.strip()).group(1) for p in params]
  out.append((typ.strip(),n,args,names))
 return out
if __name__=='__main__':
 for cls in ['IDXGIFactory7','IDXGISwapChain4']:
  print(cls)
  for t,n,a,ns in methods(cls): print(t,n,a,ns)
