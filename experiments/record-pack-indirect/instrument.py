"""Temporary source-assisted fixture edits. Backups are byte exact; never reset checkout."""
from pathlib import Path
import difflib

ROOT = Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine')
SAVE = Path('C:/Users/r3d_flzp/Desktop/ARC-perceptual/build/indirect-live-backup')
INC = '#include "C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/record-pack-indirect/fixture.hpp"\n'
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
def backend(s):
    s=INC+s
    s=replace(s,'\tvoid GraphicsDevice_DX12::SubmitCommandLists()','\tID3D12Resource* GraphicsDevice_DX12::ArcPacketResource(const GPUBuffer* b){return to_internal(b)->resource.Get();}\n\tvoid GraphicsDevice_DX12::SubmitCommandLists()')
    s=replace(s,'\t\tFRAMECOUNT++;','\t\tarc_indirect::endFrame(queues[QUEUE_GRAPHICS].queue.Get(),FRAMECOUNT);\n\t\tFRAMECOUNT++;')
    return s
edit('WickedEngine/wiGraphicsDevice_DX12.cpp',backend)
def scene(s):
    s=INC+s
    s=replace(s,'auto* resident_certificate=arc_resident_queue::begin_certificate(this);','auto* resident_certificate=arc_resident_queue::begin_certificate(this);\n        auto* indirect_side=arc_indirect::producer(this,(uint32_t)objects.GetCount());')
    s=replace(s,'[&, arc_chain_results, resident_certificate]','[&, arc_chain_results, resident_certificate, indirect_side]')
    return replace(s,'inst.SetUserStencilRef(object.userStencilRef);','inst.SetUserStencilRef(object.userStencilRef);\n                if(indirect_side)indirect_side[args.jobIndex]=arc_indirect::makeExtra(object);')
edit('WickedEngine/wiScene.cpp',scene)
edit('WickedEngine/wiRenderPath3D.cpp',lambda s:replace(INC+s,'\t\tprerender_happened = false;','\t\tprerender_happened = false;\n        arc_indirect::prepare(scene);'))
def renderer(s):
    s=INC+s
    s=replace(s,'if (vis.scene->instanceBuffer.IsValid())\n\t{\n\t\tPushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer','if (vis.scene->instanceBuffer.IsValid() && !arc_indirect::moved(vis.scene))\n\t{\n\t\tPushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer')
    s=replace(s,'if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)','if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0 && !arc_indirect::moved(vis.scene))')
    inject='''    auto* indirect_slot = !resident && stencil_usage && !wireframe ? arc_indirect::preflight(vis.scene,renderQueue.batches.data(),(uint32_t)renderQueue.size(),renderPass==RENDERPASS_MAIN?0:1):nullptr;
    if(indirect_slot){
        const auto& mesh=vis.scene->meshes[indirect_slot->group.mesh];
        bool supported=mesh.generalBuffer.IsValid() && !mesh.subsets.empty() && mesh.subsets.size()<=64 && !(mesh_shader&&mesh.vb_clu.IsValid()) && !(tessellation&&mesh.GetTessellationFactor()>0);
        for(const auto& subset:mesh.subsets)if(subset.materialIndex<vis.scene->materials.GetCount() && vis.scene->materials[subset.materialIndex].customShaderID>=0)supported=false;
        if(!supported)indirect_slot=nullptr;
        else{
            std::vector<uint32_t> templates(mesh.subsets.size()*5);
            for(uint32_t i=0;i<mesh.subsets.size();++i){auto& subset=mesh.subsets[i];const auto& ibv=mesh.ib_provoke;auto stride=mesh.GetProvokingIndexStride();uint32_t offset=uint32_t((ibv.offset+(mesh.generalBufferOffsetAllocation.IsValid()?uint64_t(mesh.generalBufferOffsetAllocation.byte_offset):0))/stride)+subset.indexOffset;templates[i*5]=subset.indexCount;templates[i*5+2]=offset;}
            if(arc_indirect::mode()==2){indirect_slot->expected.clear();for(const auto& b:renderQueue.batches){const auto& ob=vis.scene->objects[b.GetInstanceIndex()];float d=std::max(ob.GetTransparency(),std::max(0.f,b.GetDistance()-ob.fadeDistance)/ob.radius);if(d>.99f)continue;uint32_t bits=b.camera_mask;while(bits){auto bit=firstbitlow(bits);bits^=1u<<bit;ShaderMeshInstancePointer ptr;ptr.Create(b.GetInstanceIndex(),bit,d);indirect_slot->expected.push_back(ptr.data);}}}
            arc_indirect::submit(*indirect_slot,vis.scene,renderQueue.batches.data(),templates);
        }
    }
'''
    s=replace(s,'\t// Pre-allocate space for all the instances in GPU-buffer:',inject+'\t// Pre-allocate space for all the instances in GPU-buffer:')
    s=replace(s,'instances = resident ? GraphicsDevice::GPUAllocation{}','instances = (resident || indirect_slot) ? GraphicsDevice::GPUAllocation{}')
    s=replace(s,'device->GetDescriptorIndex(resident ? &resident->pointers : &instances.buffer, SubresourceType::SRV);','device->GetDescriptorIndex(indirect_slot ? &indirect_slot->output : resident ? &resident->pointers : &instances.buffer, SubresourceType::SRV);')
    start=s.index('void RenderMeshes(');end=s.index('void RenderImpostors(',start);part=s[start:end]
    old='device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, indexOffset, 0, 0, cmd);'
    assert part.count(old)==2
    part=part.replace(old,'''if(indirect_slot){if(indirect_slot->templates[subsetIndex*5]!=subset.indexCount||indirect_slot->templates[subsetIndex*5+2]!=indexOffset)throw std::runtime_error("indirect static command mismatch");device->DrawIndexedInstancedIndirect(&indirect_slot->args,subsetIndex*20,cmd);++indirect_slot->commands;}else '''+old)
    part=replace(part,'    if(resident){\n        instancedBatch.meshIndex=resident->mesh;','''    if(indirect_slot){
        auto t=arc_packet::Clock::now();const auto& g=indirect_slot->group;instancedBatch={};instancedBatch.meshIndex=g.mesh;instancedBatch.instanceCount=1;instancedBatch.dataOffset=0;instancedBatch.userStencilRefOverride=uint8_t(g.stencil);instancedBatch.lod=uint8_t(g.lod);instancedBatch.forceAlphatestForDithering=false;batch_flush();indirect_slot->retained=arc_packet::elapsed(t);device->EventEnd(cmd);return;
    }
    if(resident){
        instancedBatch.meshIndex=resident->mesh;''')
    return s[:start]+part+s[end:]
edit('WickedEngine/wiRenderer.cpp',renderer)
print('indirect instrumented; backups:',SAVE)
