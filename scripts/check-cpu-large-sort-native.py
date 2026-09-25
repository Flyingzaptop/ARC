"""Offline differential oracle: deterministic model vs original large-call bytes.

No game attachment, new trace, live replacement or FPS claim. Copy the four pinned
functions with relative addresses preserved. The only move-helper call is a
16-byte overlapping right shift in insertion sort, which cannot reach its external
forward-copy dispatch. The fixture is deliberately bound to this reviewed closure.
"""
import argparse,ctypes,hashlib,json,os,random,struct,time
from pathlib import Path
from cpu_large_sort import exact_sort
from cpu_sort_contract import key

CLOSURE='d40e3fa146ee51711c31d7b87b12e9e42f8a862bc063314e1e30257c4d07fe1a'

class Original:
    def __init__(self,blob):
        if os.name!='nt' or ctypes.sizeof(ctypes.c_void_p)!=8:raise RuntimeError('Windows x64 required')
        if hashlib.sha256(blob).hexdigest()!=CLOSURE:raise ValueError('Unreviewed closure')
        count,=struct.unpack_from('<I',blob);offset=4;parts=[]
        for _ in range(count):
            rva,n=struct.unpack_from('<II',blob,offset);offset+=8
            parts.append((rva,blob[offset:offset+n]));offset+=n
        self.k=ctypes.WinDLL('kernel32',use_last_error=True);k=self.k
        k.SetErrorMode(0x8003)
        k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.c_ulong];k.VirtualAlloc.restype=ctypes.c_void_p
        k.VirtualProtect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong)]
        k.VirtualFree.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong]
        k.GetCurrentProcess.restype=ctypes.c_void_p
        k.FlushInstructionCache.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
        self.low=min(x[0] for x in parts);size=max(r+len(b) for r,b in parts)-self.low
        self.ptr=k.VirtualAlloc(None,size,0x3000,4)
        if not self.ptr:raise ctypes.WinError(ctypes.get_last_error())
        try:
            for r,b in parts:ctypes.memmove(self.ptr+r-self.low,b,len(b))
            old=ctypes.c_ulong()
            if not k.VirtualProtect(self.ptr,size,0x20,ctypes.byref(old)):raise ctypes.WinError(ctypes.get_last_error())
            if not k.FlushInstructionCache(k.GetCurrentProcess(),self.ptr,size):raise ctypes.WinError(ctypes.get_last_error())
            self.fn=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int64,ctypes.c_uint64)(self.ptr+parts[0][0]-self.low)
            # Diagnose a pinned-fixture fault without collecting any game trace.
            self.callback_type=ctypes.WINFUNCTYPE(ctypes.c_long,ctypes.c_void_p)
            @self.callback_type
            def fault(info):
                ex,context=struct.unpack('<QQ',ctypes.string_at(info,16))
                code,=struct.unpack('<I',ctypes.string_at(ex,4))
                if code==0xc0000005:
                    rip,=struct.unpack('<Q',ctypes.string_at(context+248,8))
                    regs=struct.unpack('<16Q',ctypes.string_at(context+120,128))
                    access,address=struct.unpack('<QQ',ctypes.string_at(ex+32,16))
                    detail={'fault_rva':hex(rip-self.ptr+self.low),'access':access,'address':hex(address),'registers':[hex(x) for x in regs]}
                    os.write(2,(json.dumps(detail)+'\n').encode())
                return 0
            self.fault=fault
            k.AddVectoredExceptionHandler.argtypes=[ctypes.c_ulong,self.callback_type];k.AddVectoredExceptionHandler.restype=ctypes.c_void_p
            self.handler=k.AddVectoredExceptionHandler(1,fault)
        except BaseException:self.close();raise
    def close(self):
        if getattr(self,'handler',None):
            self.k.RemoveVectoredExceptionHandler.argtypes=[ctypes.c_void_p]
            self.k.RemoveVectoredExceptionHandler(self.handler);self.handler=None
        if self.ptr:self.k.VirtualFree(self.ptr,0,0x8000);self.ptr=None
    def run(self,data,ideal):
        if len(data)%16 or len(data)>4*1024*1024 or not 0<=ideal<=len(data)//16:raise ValueError('Input bounds')
        storage=ctypes.create_string_buffer(len(data)+64);raw=ctypes.addressof(storage);begin=(raw+31)&~15
        ctypes.memset(raw,0xA5,len(storage));ctypes.memmove(begin,data,len(data))
        prefix=ctypes.string_at(raw,begin-raw);tail=ctypes.string_at(begin+len(data),raw+len(storage)-begin-len(data))
        start=time.perf_counter_ns();self.fn(begin,begin+len(data),ideal,0);ns=time.perf_counter_ns()-start
        if prefix!=ctypes.string_at(raw,len(prefix)) or tail!=ctypes.string_at(begin+len(data),len(tail)):
            raise AssertionError('Canary corruption')
        return ctypes.string_at(begin,len(data)),ns

def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('closure',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    bits=json.loads((a.candidate/'key-hypotheses.json').read_text())['hypotheses'][0]['projection']
    original=Original(a.closure.read_bytes());results=[];rng=random.Random(250925)
    def case(name,data,ideal,expected=None):
        print('Checking',name,flush=True)
        native,ns=original.run(data,ideal);model,stats=exact_sort(data,bits,ideal)
        mismatch=sum(native[i:i+16]!=model[i:i+16] for i in range(0,len(data),16))
        saved_mismatch=None if expected is None else sum(native[i:i+16]!=expected[i:i+16] for i in range(0,len(data),16))
        result={'name':name,'records':len(data)//16,'ideal':ideal,'mismatch_records':mismatch,
                'saved_output_mismatch_records':saved_mismatch,'original_isolated_call_ns':ns,
                'input_sha256':hashlib.sha256(data).hexdigest(),'output_sha256':hashlib.sha256(native).hexdigest(),
                'model_paths':stats}
        results.append(result)
        if mismatch or saved_mismatch:raise AssertionError(result)
    try:
        real=(a.candidate/'input-span.bin').read_bytes()
        case('saved-real-65344',real,len(real)//16,(a.candidate/'output-span.bin').read_bytes())
        def encode(value,identifier):
            raw=identifier<<32
            for i,b in enumerate(bits):
                if b<0:raise ValueError('Unexpected constant')
                raw=(raw&~(1<<b))|(((value>>i)&1)<<b)
            return raw.to_bytes(16,'little')
        for n in [33,41,42,257,4096,65344,65536]:
            for pattern in ['random','equal','two-keys','ascending','descending','organ-pipe']:
                keys=([rng.getrandbits(64) for _ in range(n)] if pattern=='random' else
                      [7]*n if pattern=='equal' else [i%2 for i in range(n)] if pattern=='two-keys' else
                      list(range(n)) if pattern=='ascending' else list(range(n,0,-1)) if pattern=='descending' else
                      [min(i,n-1-i) for i in range(n)])
                data=b''.join(encode(k,i) for i,k in enumerate(keys))
                case(f'{pattern}-{n}',data,n)
                if n in [257,65344] and pattern in ['random','equal','two-keys']:
                    case(f'forced-heap-{pattern}-{n}',data,0)
    finally:original.close()
    report={'schema':1,'seed':250925,'closure_sha256':CLOSURE,'cases':results,
            'all_exact':True,'live_memory_admitted':False,'gpu_replacement':False,
            'scope':'Array-result differential oracle, not a complete equivalence proof of registers/exceptions/lifetime; isolated call times are not application savings.'}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Exact cases:',len(results),'real records:',len(real)//16)
if __name__=='__main__':main()
