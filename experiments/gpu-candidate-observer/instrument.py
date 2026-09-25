"""Wire native API events and a separate oracle into the owned fixture. Preserve originals."""
from pathlib import Path
import difflib

ROOT = Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine')
SAVE = Path('C:/Users/r3d_flzp/Desktop/ARC-perceptual/build/gpu-observer-backup')
SAVE.mkdir(exist_ok=True)
def edit(name, fn):
    p=ROOT/name;b=p.read_bytes();backup=SAVE/p.name
    if backup.exists() and backup.read_bytes()!=b: raise RuntimeError('restore first: '+name)
    backup.write_bytes(b);s=b.decode();t=fn(s.replace('\r\n','\n'))
    p.write_bytes(t.replace('\n','\r\n').encode())
    (SAVE/(p.name+'.patch')).write_text(''.join(difflib.unified_diff(s.splitlines(True),t.splitlines(True),fromfile=name,tofile=name)),encoding='utf-8')
def replace(s,a,b):
    if s.count(a)!=1:raise RuntimeError(f'anchor count {s.count(a)}: {a[:100]}')
    return s.replace(a,b)
def backend(s):
    s=replace(s,'#include "ArcWickedHooks.h"','#include "ArcWickedHooks.h"\n#include "C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/gpu-candidate-observer/observer.hpp"')
    s=replace(s,'\t\t~Resource_DX12()\n\t\t{','\t\t~Resource_DX12()\n\t\t{\n            arc_gpu_candidates::destroyed(resource.Get());')
    s=replace(s,'\t\treturn SUCCEEDED(hr);\n\t}\n\tbool GraphicsDevice_DX12::CreateTexture(', '\t\tif(SUCCEEDED(hr))arc_gpu_candidates::created(internal_state->resource.Get(),buffer->mapped_data,desc->usage==Usage::UPLOAD);\n\t\treturn SUCCEEDED(hr);\n\t}\n\tbool GraphicsDevice_DX12::CreateTexture(')
    s=replace(s,'\t\tARCWickedCommandBegin(commandlist.GetCommandList(), queues[queue].desc.Type);','\t\tarc_gpu_candidates::begin(commandlist.GetCommandList());\n\t\tARCWickedCommandBegin(commandlist.GetCommandList(), queues[queue].desc.Type);')
    s=replace(s,'\t\tARCWickedSubmit(queue.Get(), submit_cmds.data(), submit_cmds.size(), desc.Type);','\t\tarc_gpu_candidates::submitted(queue.Get(),submit_cmds.data(),submit_cmds.size());\n\t\tARCWickedSubmit(queue.Get(), submit_cmds.data(), submit_cmds.size(), desc.Type);')
    s=replace(s,'\t\tcommandlist.GetGraphicsCommandList()->CopyBufferRegion(dst_internal->resource.Get(), dst_offset, src_internal->resource.Get(), src_offset, size);','\t\tcommandlist.GetGraphicsCommandList()->CopyBufferRegion(dst_internal->resource.Get(), dst_offset, src_internal->resource.Get(), src_offset, size);\n        arc_gpu_candidates::copy(commandlist.GetGraphicsCommandList(),dst_internal->resource.Get(),dst_offset,src_internal->resource.Get(),src_offset,size);')
    s=replace(s,'\t\tif (!barrierdescs.empty())\n\t\t{','\t\tif (!barrierdescs.empty())\n\t\t{\n            arc_gpu_candidates::barriers(barrierdescs.data(),barrierdescs.size());')
    s=replace(s,'\t\tif (resource != nullptr && resource->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(resource)->resource.Get(), false);','\t\tif(resource&&resource->IsValid())arc_gpu_candidates::used(to_internal(resource)->resource.Get());\n\t\tif (resource != nullptr && resource->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(resource)->resource.Get(), false);')
    s=replace(s,'\tvoid GraphicsDevice_DX12::SubmitCommandLists()', '\textern "C" uint64_t ARCAuditNativeResource(const GPUResource* r){return (uintptr_t)to_internal(r)->resource.Get();}\n\tvoid GraphicsDevice_DX12::SubmitCommandLists()')
    return s
edit('WickedEngine/wiGraphicsDevice_DX12.cpp',backend)
def oracle(s):
    s='#include "C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/gpu-candidate-observer/oracle.hpp"\n'+s
    return replace(s,'if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)\n\t{','if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)\n\t{\n        arc_candidate_oracle::observe(*vis.scene);')
edit('WickedEngine/wiRenderer.cpp',oracle)
