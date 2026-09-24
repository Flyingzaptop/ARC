"""Streaming/lossless reader for bounded ARC training records."""
import json,struct
from pathlib import Path

def event_records(path,meta,vectors=False):
 if not meta.get('streaming'):
  with (path/'events.jsonl').open() as f:
   for line in f:yield json.loads(line)
  return
 size=meta['event_bytes'];words=size//8;mask_bytes=(words+7)//8;state=[0]*words;mask_cache={}
 def field(key,fmt):
  off=meta[key];value=state[off//8]>>(8*(off%8));return value&((1<<(32 if fmt=='<I' else 64))-1)
 with (path/'events.bin').open('rb',buffering=1024*1024) as f:
  for index in range(meta['written_events']):
   if meta.get('stream_encoding')=='changed_words_v1':
    mask=f.read(mask_bytes)
    if len(mask)!=mask_bytes:raise ValueError('Truncated change mask')
    cached=mask_cache.get(mask)
    if cached is None:
     positions=[byte*8+bit for byte,bits in enumerate(mask) for bit in range(8) if bits&(1<<bit)]
     if positions and positions[-1]>=words:raise ValueError('Invalid change mask')
     cached=(positions,struct.Struct('<'+'Q'*len(positions)))
     if len(mask_cache)<4096:mask_cache[mask]=cached
    positions,unpack=cached;data=f.read(unpack.size)
    if len(data)!=unpack.size:raise ValueError('Truncated changed words')
    for pos,value in zip(positions,unpack.unpack(data)):state[pos]=value
   else:
    data=f.read(size)
    if len(data)!=size:raise ValueError('Truncated native event')
    state[:]=struct.unpack('<'+'Q'*words,data)
   code_size=field('code_size_offset','<I')
   if code_size>16:raise ValueError('Invalid instruction byte count')
   kind=field('kind_offset','<I')
   e={'index':index,'kind':kind,'tid':field('tid_offset','<I'),'qpc':field('qpc_offset','<Q'),'teb':field('teb_offset','<Q'),'rip':field('rip_offset','<Q'),'eflags':field('eflags_offset','<I'),'mxcsr':field('mxcsr_offset','<I'),'registers':state[meta['rax_offset']//8:meta['rax_offset']//8+16],'code':struct.pack('<QQQ',*state[meta['code_offset']//8:meta['code_offset']//8+3])[meta['code_offset']%8:meta['code_offset']%8+code_size].hex()}
   if 'last_error_offset' in meta:e['last_error']=field('last_error_offset','<I');e['last_status']=field('last_status_offset','<I')
   if kind in [1,2]:e['raw_context_hex']=struct.pack('<'+'Q'*(meta['context_bytes']//8),*state[:meta['context_bytes']//8]).hex()
   if vectors:e['xmm_bytes']=struct.pack('<'+'Q'*32,*state[meta['xmm_offset']//8:meta['xmm_offset']//8+32])
   yield e
  if f.read(1):raise ValueError('Unexpected data after declared stream')
