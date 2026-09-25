"""Temporary source-assisted fixture edits. Backups are byte exact; never reset checkout."""
from pathlib import Path
import difflib

ROOT = Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine')
SAVE = Path('C:/Users/r3d_flzp/Desktop/ARC-perceptual/build/packet-live-backup')
INC = '#include "C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/record-pack-gpu/fixture.hpp"\n'
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
edit('WickedEngine/wiGraphicsDevice_DX12.h',lambda s:replace(s,'\tpublic:\n','\tpublic:\n\t\tID3D12Device* ArcPacketDevice(){return device.Get();}\n\t\tID3D12CommandQueue* ArcPacketQueue(){return queues[QUEUE_GRAPHICS].queue.Get();}\n\t\tID3D12Resource* ArcPacketResource(const GPUBuffer* b);\n'))
edit('WickedEngine/wiGraphicsDevice_DX12.cpp',lambda s:replace(s,'\tvoid GraphicsDevice_DX12::SubmitCommandLists()','\tID3D12Resource* GraphicsDevice_DX12::ArcPacketResource(const GPUBuffer* b){return to_internal(b)->resource.Get();}\n\tvoid GraphicsDevice_DX12::SubmitCommandLists()'))
def scene(s):
    s=INC+s
    s=replace(s,'auto* resident_certificate=arc_resident_queue::begin_certificate(this);','auto* resident_certificate=arc_resident_queue::begin_certificate(this);\n        auto* packet_side=arc_packet_fixture::producer(this,(uint32_t)objects.GetCount());')
    s=replace(s,'[&, arc_chain_results, resident_certificate]','[&, arc_chain_results, resident_certificate, packet_side]')
    s=replace(s,'inst.SetUserStencilRef(object.userStencilRef);','inst.SetUserStencilRef(object.userStencilRef);\n                if(packet_side)packet_side[args.jobIndex]={object.GetTransparency(),object.lod,uint32_t(object.alphaRef<1)};')
    return s
edit('WickedEngine/wiScene.cpp',scene)
edit('WickedEngine/wiRenderPath3D.cpp',lambda s:replace(INC+s,'\t\tprerender_happened = false;','\t\tprerender_happened = false;\n        arc_packet_fixture::prepare(scene);'))
def renderer(s):
    s=INC+s
    s=replace(s,'if (vis.scene->instanceBuffer.IsValid())\n\t{\n\t\tPushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer','if (vis.scene->instanceBuffer.IsValid() && !arc_packet_fixture::moved(vis.scene))\n\t{\n\t\tPushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer')
    s=replace(s,'if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)','if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0 && !arc_packet_fixture::moved(vis.scene))')
    anchor='\t// Pre-allocate space for all the instances in GPU-buffer:'
    inject='''    auto* packet_slot = !resident && stencil_usage ? arc_packet_fixture::run(vis.scene,renderQueue.batches.data(),(uint32_t)renderQueue.size(),renderPass==RENDERPASS_MAIN?0:1):nullptr;
    if(packet_slot && arc_packet_fixture::mode()==1){
        arc_packet::Result ref{};std::vector<uint32_t> words;uint32_t i=0;
        for(const auto& b:renderQueue.batches){const auto& ob=vis.scene->objects[b.GetInstanceIndex()];uint32_t lod=b.lod_override==255?uint8_t(ob.lod):b.lod_override;
            if(i==0||ref.groups[ref.count-1].mesh!=b.GetMeshIndex()||ref.groups[ref.count-1].stencil!=ob.userStencilRef||ref.groups[ref.count-1].lod!=lod){
                if(ref.count==256)throw std::runtime_error("oracle group overflow");if(ref.count)ref.groups[ref.count-1].end=i;
                ref.groups[ref.count++]={b.GetMeshIndex(),lod,ob.userStencilRef,0,(uint32_t)words.size(),0,i,(uint32_t)renderQueue.size()};}
            ++i;auto& g=ref.groups[ref.count-1];float d=std::max(ob.GetTransparency(),std::max(0.f,b.GetDistance()-ob.fadeDistance)/ob.radius);if(d>.99f)continue;
            if(d>0||ob.alphaRef<1)g.alpha=1;uint32_t bits=b.camera_mask;while(bits){uint32_t bit=firstbitlow(bits);bits^=1u<<bit;ShaderMeshInstancePointer ptr;ptr.Create(b.GetInstanceIndex(),bit,d);words.push_back(ptr.data);++g.count;}
        }
        ref.words=(uint32_t)words.size();const auto& got=packet_slot->result;
        if(got.error||got.count!=ref.count||got.words!=ref.words||memcmp(got.groups,ref.groups,ref.count*sizeof(arc_packet::Group)))throw std::runtime_error("live packet metadata mismatch");
        void* data{};arc_packet::check(packet_slot->worker.verify->Map(0,nullptr,&data));bool equal=memcmp(data,words.data(),words.size()*4)==0;packet_slot->worker.verify->Unmap(0,nullptr);if(!equal)throw std::runtime_error("live packet words mismatch");
        packet_slot->validated=(uint32_t)renderQueue.size();packet_slot=nullptr;
    }
'''
    s=replace(s,anchor,inject+anchor)
    s=replace(s,'instances = resident ? GraphicsDevice::GPUAllocation{}','instances = (resident || packet_slot) ? GraphicsDevice::GPUAllocation{}')
    s=replace(s,'device->GetDescriptorIndex(resident ? &resident->pointers : &instances.buffer, SubresourceType::SRV);','device->GetDescriptorIndex(packet_slot ? &packet_slot->output : resident ? &resident->pointers : &instances.buffer, SubresourceType::SRV);')
    s=replace(s,'    if(resident){\n        instancedBatch.meshIndex=resident->mesh;', '''    if(packet_slot){
        auto start=arc_packet::Clock::now();
        for(uint32_t i=0;i<packet_slot->result.count;++i){const auto& g=packet_slot->result.groups[i];instancedBatch={};instancedBatch.meshIndex=g.mesh;instancedBatch.instanceCount=g.count;instancedBatch.dataOffset=g.offset*4;instancedBatch.userStencilRefOverride=uint8_t(g.stencil);instancedBatch.lod=uint8_t(g.lod);instancedBatch.forceAlphatestForDithering=g.alpha!=0;batch_flush();}
        packet_slot->retained=arc_packet::elapsed(start);packet_slot->replaced=(uint32_t)renderQueue.size();device->EventEnd(cmd);return;
    }
    if(resident){
        instancedBatch.meshIndex=resident->mesh;''')
    s=replace(s,'\tauto batch_flush = [&]()\n\t{','\tdouble packet_flush_ms=0;\n\tauto batch_flush = [&]()\n\t{\n        arc_packet_fixture::FlushTimer packet_timer{packet_flush_ms};')
    s=replace(s,'\tuint32_t instanceCount = 0;\n\tfor (const RenderBatch& batch','\tuint32_t instanceCount = 0;\n    auto packet_original_start=arc_packet::Clock::now();\n\tfor (const RenderBatch& batch')
    start=s.index('void RenderMeshes(');end=s.index('void RenderImpostors(',start)
    part=replace(s[start:end],'\tbatch_flush();\n\n\tdevice->EventEnd(cmd);','\tbatch_flush();\n    if(stencil_usage)arc_packet_fixture::cpuCost(renderPass==RENDERPASS_MAIN?0:1,arc_packet::elapsed(packet_original_start)-packet_flush_ms,packet_flush_ms);\n\n\tdevice->EventEnd(cmd);')
    return s[:start]+part+s[end:]
edit('WickedEngine/wiRenderer.cpp',renderer)
print('instrumented; backups:',SAVE)
