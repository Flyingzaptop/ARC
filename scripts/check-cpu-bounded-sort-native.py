"""Isolated original-code oracle for the restricted insertion path.

Never attaches to an application. This is a pinned diagnostic fixture, not a
detector or live replacement. Relative calls in the copied function are not
relocated: the input guards restrict execution to the call-free insertion path.
Run as a separate bounded process; a failed assertion/crash invalidates evidence.
"""
import argparse, ctypes, hashlib, json, os, random, time
from pathlib import Path
from cpu_sort_contract import preflight, key

CODE_SHA256='4e76216e6202ba5f56139379244833faa9a594f256129d2ea4eca07e251fad8a'

def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if os.name!='nt':raise SystemExit('Windows x64 only')
    code=(a.candidate/'expected-code.bin').read_bytes()
    if hashlib.sha256(code).hexdigest()!=CODE_SHA256:raise SystemExit('Unreviewed machine-code generation')
    bits=json.loads((a.candidate/'key-hypotheses.json').read_text())['hypotheses'][0]['projection']
    k=ctypes.WinDLL('kernel32',use_last_error=True);k.SetErrorMode(0x0001|0x0002|0x8000)
    k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.c_ulong];k.VirtualAlloc.restype=ctypes.c_void_p
    k.VirtualProtect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong)]
    k.VirtualFree.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong]
    k.GetCurrentProcess.restype=ctypes.c_void_p
    k.FlushInstructionCache.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
    ptr=k.VirtualAlloc(None,len(code),0x3000,4)
    if not ptr:raise ctypes.WinError(ctypes.get_last_error())
    tests=0;rng=random.Random(4017);start=time.perf_counter()
    try:
        ctypes.memmove(ptr,code,len(code));old=ctypes.c_ulong()
        if not k.VirtualProtect(ptr,len(code),0x20,ctypes.byref(old)):raise ctypes.WinError(ctypes.get_last_error())
        if not k.FlushInstructionCache(k.GetCurrentProcess(),ptr,len(code)):raise ctypes.WinError(ctypes.get_last_error())
        fn=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int64,ctypes.c_uint64)(ptr)
        for n in range(2,33):
            for trial in range(24):
                keys=[]
                while len(keys)<n:
                    value=rng.getrandbits(63)
                    if value not in keys:keys.append(value)
                if trial%3==0:keys=[0]+[x|(1<<63) for x in keys[1:]]
                records=[]
                for value in keys:
                    raw=rng.getrandbits(128)
                    for pos,bit in enumerate(bits):
                        if bit<0:raise ValueError('Unexpected projection constant in pinned fixture')
                        raw=(raw & ~(1<<bit)) | (((value>>pos)&1)<<bit)
                    records.append(raw.to_bytes(16,'little'))
                if trial%4==0:records[-1]=records[0]  # exact duplicate, no distinct-payload tie
                minimum=min(range(n),key=lambda i:key(records[i],bits))
                records[0],records[minimum]=records[minimum],records[0]
                original=b''.join(records)
                assert preflight(original,bits)['input_guards_pass']
                storage=ctypes.create_string_buffer(len(original)+64)
                begin=(ctypes.addressof(storage)+31)&~15
                ctypes.memset(ctypes.addressof(storage),0xA5,len(storage))
                ctypes.memmove(begin,original,len(original))
                prefix=ctypes.string_at(ctypes.addressof(storage),begin-ctypes.addressof(storage))
                tail=ctypes.string_at(begin+len(original),ctypes.addressof(storage)+len(storage)-begin-len(original))
                fn(begin,begin+len(original),n,0)
                assert ctypes.string_at(begin,len(original))==b''.join(sorted(records,key=lambda r:key(r,bits)))
                assert prefix==ctypes.string_at(ctypes.addressof(storage),len(prefix))
                assert tail==ctypes.string_at(begin+len(original),len(tail))
                tests+=1
    finally:
        k.VirtualFree(ptr,0,0x8000)
    result={'schema':1,'original_code_sha256':CODE_SHA256,'native_cases':tests,
            'minimum_records':2,'maximum_records':32,'seed':4017,'elapsed_ms':(time.perf_counter()-start)*1000,
            'byte_output_mismatches':0,'canary_failures':0,'gpu_replacement':False,
            'scope':'Isolated original binary path oracle; no application ownership, full ABI or all-input proof; not a speed benchmark'}
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
