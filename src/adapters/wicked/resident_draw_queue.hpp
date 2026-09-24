#pragma once
// Source-assisted homogeneous opaque draw batch. Not universal ARC admission.
#include "wiScene.h"
#include "wiRenderer.h"
#include "wiGraphicsDevice.h"
#include "wiHelper.h"
#include "wiProfiler.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>
extern "C" void ARCWickedCpuSample(const char*,double) noexcept;
extern "C" void ARCWickedFrameSample(const char*,double,uint64_t) noexcept;
namespace arc_resident_queue {
using namespace wi::graphics;
using Clock=std::chrono::steady_clock;
using Api=DWORD(WINAPI*)(void*);
inline std::atomic<Api> session_enabled{nullptr},shader_scope{nullptr},work_scope{nullptr};
inline std::atomic<bool> session_bound{false};
inline int mode(){
    static const int requested=[] {auto p=std::getenv("ARC_RESIDENT_QUEUE");return p?std::atoi(p):0;}();
    auto query=session_enabled.load(std::memory_order_acquire);
    return session_bound.load(std::memory_order_acquire)&&query&&query(nullptr)==1?requested:0;
}
inline void bind_session(Api enabled,Api scope,Api work){work_scope.store(work);shader_scope.store(scope);session_enabled.store(enabled);session_bound.store(enabled&&scope&&work);}
inline void unbind_session(){session_bound=false;session_enabled=nullptr;shader_scope=nullptr;}
struct ShaderScope{Api scope=shader_scope.load();ShaderScope(){if(scope&&scope(reinterpret_cast<void*>(1)))fail_scope();}~ShaderScope(){if(scope)scope(nullptr);}static void fail_scope(){ExitProcess(97);}};
struct WorkScope{Api api=work_scope.load();WorkScope(){if(api)api(reinterpret_cast<void*>(1));}~WorkScope(){if(api)api(nullptr);}};
inline thread_local bool force_cpu=false;
inline void fail(const char* why,uint64_t a=0,uint64_t b=0){
    if(auto p=std::getenv("ARC_WICKED_CPU_PROFILE")){auto name=std::string(p)+".resident-error.txt";FILE* f{};fopen_s(&f,name.c_str(),"w");if(f){std::fprintf(f,"%s: %llu %llu\n",why,a,b);std::fclose(f);}}
    ExitProcess(98);
}
struct Certificate { const wi::scene::Scene* scene{};uint64_t frame{};uint32_t mesh{},lod{},stencil{};DirectX::XMFLOAT3 eye{};std::atomic<bool> valid{false}; };
inline Certificate certificate;
struct Entry { uint32_t bits,distance,index,padding; };
inline bool less(const Entry& a,const Entry& b){if(a.bits!=b.bits)return a.bits<b.bits;if(a.distance!=b.distance)return a.distance<b.distance;return a.index<b.index;}
struct Slot {
    GPUBuffer entries,pointers,entry_readback,pointer_readback,pixel_counts,pixel_readback;
    Texture reference_id,reference_depth,depth_copy;
    GraphicsDevice::GPUAllocation input;
    const wi::renderer::Visibility* visibility{};
    uint64_t frame=~0ull;
    uint32_t capacity{},count{},padded{},mesh{},lod{},stencil{},representative{};
    bool prepared{},pointer_srv{},pending{},pixels_pending{};
    std::atomic<bool> expected_claimed{false};
    std::atomic<uint32_t> uses{0};
    std::vector<Entry> expected;
};
inline std::vector<std::unique_ptr<Slot>> slots;
inline Shader queue_shader,pixel_shader;
inline std::atomic<uint64_t> skipped_build{0},skipped_sort{0},skipped_pack{0},prepare_ns{0},record_ns{0},upload_bytes{0};
inline double milliseconds(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
inline void report(){
    if(!session_bound.load())return;
    const auto frame=certificate.frame;
    auto sample=[&](const char* name,double value){ARCWickedFrameSample(name,value,frame);};
    sample("Resident permission",mode()?1:0);
    sample("Resident certificate valid",certificate.valid?1:0);
    sample("Resident previous build items skipped",double(skipped_build.exchange(0)));
    sample("Resident previous sort items skipped",double(skipped_sort.exchange(0)));
    sample("Resident previous pack items skipped",double(skipped_pack.exchange(0)));
    sample("Resident previous prepare ms",double(prepare_ns.exchange(0))/1e6);
    sample("Resident previous GPU record ms",double(record_ns.exchange(0))/1e6);
    sample("Resident previous upload bytes",double(upload_bytes.exchange(0)));
}
inline Certificate* begin_certificate(wi::scene::Scene* scene){
    if(scene!=&wi::scene::GetScene())return nullptr;
    report();if(!mode()){certificate.frame=GetDevice()->GetFrameCount();return nullptr;}certificate.scene=scene;certificate.frame=GetDevice()->GetFrameCount();certificate.eye=scene->camera.Eye;
    bool ok=scene->objects.GetCount()>0 && scene->objects.GetCount()<(1u<<24) && scene->hairs.GetCount()==0 && scene->emitters.GetCount()==0 && scene->impostors.GetCount()==0;
    if(ok){auto& o=scene->objects[0];certificate.mesh=o.mesh_index;certificate.lod=o.lod;certificate.stencil=o.userStencilRef;ok=certificate.mesh<scene->meshes.GetCount();}
    certificate.valid.store(ok);return &certificate;
}
inline void observe(Certificate* c,const wi::scene::ObjectComponent& o,const wi::scene::MeshComponent& mesh,bool softbody){
    if(!c||!c->valid.load(std::memory_order_relaxed))return;
    bool ok=o.mesh_index==c->mesh && o.lod==c->lod && o.userStencilRef==c->stencil &&
        o.IsRenderable() && !o.IsForeground() && !o.IsNotVisibleInMainCamera() &&
        o.GetFilterMask()==wi::enums::FILTER_OPAQUE && o.alphaRef==1 && o.color.w==1 &&
        o.fadeDistance==std::numeric_limits<float>::max() && !mesh.IsSkinned() && !mesh.IsDynamic() && !softbody &&
        std::isfinite(o.radius) && o.radius>0 && std::isfinite(o.center.x)&&std::isfinite(o.center.y)&&std::isfinite(o.center.z) &&
        std::abs(o.center.x)<1e4f&&std::abs(o.center.y)<1e4f&&std::abs(o.center.z)<1e4f;
    float dx=std::abs(o.center.x-c->eye.x),dy=std::abs(o.center.y-c->eye.y),dz=std::abs(o.center.z-c->eye.z);
    ok=ok&&((dx==0&&dy==0&&dz==0)||std::max(dx,std::max(dy,dz))>=0.000244140625f);
    if(!ok)c->valid.store(false,std::memory_order_relaxed);
}
inline void load_shader(Shader& shader,const char* name){
    ShaderScope own_shader;wi::vector<uint8_t> code;if(!wi::helper::FileRead(wi::renderer::GetShaderPath()+name,code)||!GetDevice()->CreateShader(ShaderStage::CS,code.data(),code.size(),&shader))fail("shader load");
}
inline GPUBuffer readback(uint64_t bytes){GPUBuffer result;GPUBufferDesc d;d.usage=Usage::READBACK;d.size=bytes;if(!GetDevice()->CreateBuffer(&d,nullptr,&result))fail("readback allocation");return result;}
inline void verify(Slot& s){
    if(mode()!=2||!s.pending)return;
    if(!s.uses.load()||!s.expected_claimed.load()||s.expected.size()!=s.count||!s.pixels_pending)fail("oracle consumer coverage");
    auto* e=static_cast<const Entry*>(s.entry_readback.mapped_data);auto* p=static_cast<const uint32_t*>(s.pointer_readback.mapped_data);
    for(uint32_t i=0;i<s.count;++i){if(e[i].bits!=s.expected[i].bits||e[i].distance!=s.expected[i].distance||e[i].index!=s.expected[i].index||e[i].padding!=s.expected[i].padding){
            if(auto path=std::getenv("ARC_WICKED_CPU_PROFILE")){auto name=std::string(path)+".queue-diff.csv";FILE* file{};fopen_s(&file,name.c_str(),"w");if(file){
                std::fprintf(file,"position,gpu_bits,gpu_half,gpu_id,cpu_bits,cpu_half,cpu_id,cpu_half_for_gpu_id,gpu_float_bits,cpu_float_bits\n");
                for(uint32_t row=i;row<std::min(s.count,i+16);++row){auto match=std::find_if(s.expected.begin(),s.expected.end(),[&](const Entry& x){return x.index==e[row].index;});std::fprintf(file,"%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",row,e[row].bits,e[row].distance,e[row].index,s.expected[row].bits,s.expected[row].distance,s.expected[row].index,match==s.expected.end()?~0u:match->distance,e[row].padding,match==s.expected.end()?~0u:match->padding);}
                auto pix=static_cast<const uint32_t*>(s.pixel_readback.mapped_data);std::fprintf(file,"pixels,%u,%u,%u\n",pix[0],pix[1],pix[2]);std::fclose(file);}}
            fail("exact sort key or membership",i,e[i].index);
        }if(p[i]!=(e[i].index&0xffffff))fail("consumer pointer",i,p[i]);}
    auto* pixel=static_cast<const uint32_t*>(s.pixel_readback.mapped_data);
    if(pixel[0]||pixel[1])fail("exact prepass ID/depth pixels",pixel[0],pixel[1]);
    ARCWickedCpuSample("Resident completed exact records",s.count);
    ARCWickedCpuSample("Resident completed pixel errors",0);
    ARCWickedCpuSample("Resident completed nonzero pixels",pixel[2]);
    ARCWickedCpuSample("Resident oracle width",s.reference_id.desc.width);
    ARCWickedCpuSample("Resident oracle height",s.reference_id.desc.height);
    if(!pixel[2])fail("empty pixel oracle");
}
inline Slot* prepare(const wi::renderer::Visibility& vis,CommandList cmd){
    if(!mode()||!certificate.valid.load()||certificate.scene!=vis.scene||certificate.frame!=GetDevice()->GetFrameCount()||wi::renderer::GetOcclusionCullingEnabled())return nullptr;
    const auto n=vis.visibleObjects.size();if(!n||n>(1u<<20)||!vis.scene->instanceBuffer.IsValid())return nullptr;
    auto eye=vis.camera->Eye;if(!std::isfinite(eye.x)||!std::isfinite(eye.y)||!std::isfinite(eye.z)||std::abs(eye.x)>=1e4f||std::abs(eye.y)>=1e4f||std::abs(eye.z)>=1e4f||std::memcmp(&eye,&certificate.eye,sizeof(eye))!=0)return nullptr;
    auto start=Clock::now();auto* device=GetDevice();
    if(slots.empty()){for(uint32_t i=0;i<device->GetBufferCount();++i)slots.push_back(std::make_unique<Slot>());load_shader(queue_shader,"arcResidentQueueCS.cso");if(mode()==2)load_shader(pixel_shader,"arcResidentPixelsCS.cso");}
    auto& s=*slots[device->GetBufferIndex()];if(s.frame==device->GetFrameCount())return nullptr;
    verify(s);s.pending=false;s.pixels_pending=false;s.expected_claimed=false;s.uses=0;s.expected.clear();
    s.count=uint32_t(n);s.padded=1;while(s.padded<s.count)s.padded*=2;
    if(s.capacity<s.padded){
        GPUBufferDesc d;d.size=uint64_t(s.padded)*16;d.stride=16;d.bind_flags=BindFlag::SHADER_RESOURCE|BindFlag::UNORDERED_ACCESS;d.misc_flags=ResourceMiscFlag::BUFFER_STRUCTURED;
        if(!device->CreateBuffer(&d,nullptr,&s.entries))fail("entry allocation");
        d.size=uint64_t(s.padded)*4;d.stride=0;d.misc_flags=ResourceMiscFlag::BUFFER_RAW;if(!device->CreateBuffer(&d,nullptr,&s.pointers))fail("pointer allocation");
        if(mode()==2){s.entry_readback=readback(uint64_t(s.padded)*16);s.pointer_readback=readback(uint64_t(s.padded)*4);}
        s.capacity=s.padded;s.pointer_srv=false;
    }
    s.input=device->AllocateGPU(n*8,cmd);
    struct Input {uint32_t index,bits;};auto* dest=static_cast<Input*>(s.input.data);
    for(size_t i=0;i<n;++i){auto index=vis.visibleObjects[i];Input input{index,vis.scene->objects[index].sort_bits&0xffffff};std::memcpy(dest+i,&input,sizeof(input));}
    s.visibility=&vis;s.frame=device->GetFrameCount();s.mesh=certificate.mesh;s.lod=certificate.lod;s.stencil=certificate.stencil;s.representative=vis.visibleObjects[0];s.prepared=true;
    upload_bytes.fetch_add(n*8);prepare_ns.fetch_add(uint64_t(milliseconds(start)*1e6));return &s;
}
inline void record(Slot* s,CommandList cmd){
    if(!s)return;WorkScope own_work;auto start=Clock::now();auto* device=GetDevice();
    auto gpu_range=wi::profiler::BeginRangeGPU("ResidentQueue GPU",cmd);
    if(s->pointer_srv){auto b=GPUBarrier::Buffer(&s->pointers,ResourceState::SHADER_RESOURCE,ResourceState::UNORDERED_ACCESS);device->Barrier(&b,1,cmd);}
    struct Params {DirectX::XMFLOAT3 eye;uint32_t count,offset,padded,j,k;} p{s->visibility->camera->Eye,s->count,uint32_t(s->input.offset),s->padded,0,0};static_assert(sizeof(p)==32);
    device->BindComputeShader(&queue_shader,cmd);device->BindResource(&s->visibility->scene->instanceBuffer,0,cmd);device->BindResource(&s->input.buffer,1,cmd);device->BindUAV(&s->entries,0,cmd);device->BindUAV(&s->pointers,1,cmd);
    auto launch=[&]{device->PushConstants(&p,sizeof(p),cmd);device->Dispatch((s->padded+63)/64,1,1,cmd);auto b=GPUBarrier::Memory(&s->entries);device->Barrier(&b,1,cmd);};
    launch();for(p.k=2;p.k<=s->padded;p.k*=2)for(p.j=p.k/2;p.j;p.j/=2)launch();
    p.j=0;p.k=1;launch();
    if(mode()==2){
        GPUBarrier b[]={GPUBarrier::Buffer(&s->entries,ResourceState::UNORDERED_ACCESS,ResourceState::COPY_SRC),GPUBarrier::Buffer(&s->pointers,ResourceState::UNORDERED_ACCESS,ResourceState::COPY_SRC)};device->Barrier(b,2,cmd);
        device->CopyBuffer(&s->entry_readback,0,&s->entries,0,uint64_t(s->count)*16,cmd);device->CopyBuffer(&s->pointer_readback,0,&s->pointers,0,uint64_t(s->count)*4,cmd);
        b[0]=GPUBarrier::Buffer(&s->entries,ResourceState::COPY_SRC,ResourceState::UNORDERED_ACCESS);b[1]=GPUBarrier::Buffer(&s->pointers,ResourceState::COPY_SRC,ResourceState::SHADER_RESOURCE);device->Barrier(b,2,cmd);s->pending=true;
    }else{auto b=GPUBarrier::Buffer(&s->pointers,ResourceState::UNORDERED_ACCESS,ResourceState::SHADER_RESOURCE);device->Barrier(&b,1,cmd);}
    wi::profiler::EndRange(gpu_range);s->pointer_srv=true;record_ns.fetch_add(uint64_t(milliseconds(start)*1e6));
}
inline Slot* lookup(const wi::renderer::Visibility& vis,wi::enums::RENDERPASS pass,uint32_t flags){
    using namespace wi::renderer;
    if(!mode()||force_cpu||(_mm_getcsr()&0x6000u)!=0||slots.empty()||!(flags&DRAWSCENE_OPAQUE)||(flags&DRAWSCENE_TRANSPARENT)||(flags&DRAWSCENE_FOREGROUND_ONLY)||!(flags&DRAWSCENE_MAINCAMERA)||(flags&DRAWSCENE_SKIP_PLANAR_REFLECTION_OBJECTS))return nullptr;
    if(pass!=wi::enums::RENDERPASS_PREPASS&&pass!=wi::enums::RENDERPASS_PREPASS_DEPTHONLY&&pass!=wi::enums::RENDERPASS_MAIN)return nullptr;
    auto& s=*slots[GetDevice()->GetBufferIndex()];return s.prepared&&s.frame==GetDevice()->GetFrameCount()&&s.visibility==&vis?&s:nullptr;
}
inline void pixels(Slot* s,const Texture& actual_id,const Texture& actual_depth,const wi::renderer::Visibility& vis,uint32_t flags,CommandList cmd){
    if(mode()!=2||!s)return;auto* device=GetDevice();
    if(actual_id.desc.sample_count!=1||actual_depth.desc.sample_count!=1)fail("pixel oracle MSAA unsupported");
    if(s->reference_id.desc.width!=actual_id.desc.width||s->reference_id.desc.height!=actual_id.desc.height){
        auto d=actual_id.desc;d.misc_flags=ResourceMiscFlag::NONE;if(!device->CreateTexture(&d,nullptr,&s->reference_id))fail("reference ID allocation");d=actual_depth.desc;d.misc_flags=ResourceMiscFlag::NONE;d.bind_flags|=BindFlag::SHADER_RESOURCE;if(!device->CreateTexture(&d,nullptr,&s->reference_depth))fail("reference depth allocation");d.layout=ResourceState::SHADER_RESOURCE_COMPUTE;if(!device->CreateTexture(&d,nullptr,&s->depth_copy))fail("readable depth allocation");
        GPUBufferDesc bd;bd.size=16;bd.bind_flags=BindFlag::UNORDERED_ACCESS;bd.misc_flags=ResourceMiscFlag::BUFFER_RAW;if(!device->CreateBuffer(&bd,nullptr,&s->pixel_counts))fail("pixel counter allocation");s->pixel_readback=readback(16);
    }
    RenderPassImage targets[]={RenderPassImage::DepthStencil(&s->reference_depth,RenderPassImage::LoadOp::CLEAR,RenderPassImage::StoreOp::STORE,ResourceState::DEPTHSTENCIL,ResourceState::DEPTHSTENCIL,ResourceState::DEPTHSTENCIL),RenderPassImage::RenderTarget(&s->reference_id,RenderPassImage::LoadOp::CLEAR,RenderPassImage::StoreOp::STORE,ResourceState::SHADER_RESOURCE_COMPUTE,ResourceState::SHADER_RESOURCE_COMPUTE)};
    device->RenderPassBegin(targets,2,cmd);force_cpu=true;wi::renderer::DrawScene(vis,wi::enums::RENDERPASS_PREPASS,cmd,flags);force_cpu=false;device->RenderPassEnd(cmd);
    GPUBarrier b[]={GPUBarrier::Image(&actual_depth,ResourceState::DEPTHSTENCIL,ResourceState::COPY_SRC),GPUBarrier::Image(&s->depth_copy,ResourceState::SHADER_RESOURCE_COMPUTE,ResourceState::COPY_DST),GPUBarrier::Image(&s->reference_depth,ResourceState::DEPTHSTENCIL,ResourceState::SHADER_RESOURCE_COMPUTE)};device->Barrier(b,3,cmd);
    device->CopyResource(&s->depth_copy,&actual_depth,cmd);
    b[0]=GPUBarrier::Image(&actual_depth,ResourceState::COPY_SRC,ResourceState::DEPTHSTENCIL);b[1]=GPUBarrier::Image(&s->depth_copy,ResourceState::COPY_DST,ResourceState::SHADER_RESOURCE_COMPUTE);device->Barrier(b,2,cmd);
    device->ClearUAV(&s->pixel_counts,0,cmd);auto memory=GPUBarrier::Memory(&s->pixel_counts);device->Barrier(&memory,1,cmd);
    device->BindComputeShader(&pixel_shader,cmd);device->BindResource(&actual_id,0,cmd);device->BindResource(&s->reference_id,1,cmd);device->BindResource(&s->depth_copy,2,cmd);device->BindResource(&s->reference_depth,3,cmd);device->BindUAV(&s->pixel_counts,0,cmd);
    uint32_t size[]={actual_id.desc.width,actual_id.desc.height};device->PushConstants(size,sizeof(size),cmd);device->Dispatch((size[0]+15)/16,(size[1]+15)/16,1,cmd);
    auto copy=GPUBarrier::Buffer(&s->pixel_counts,ResourceState::UNORDERED_ACCESS,ResourceState::COPY_SRC);device->Barrier(&copy,1,cmd);device->CopyBuffer(&s->pixel_readback,0,&s->pixel_counts,0,16,cmd);copy=GPUBarrier::Buffer(&s->pixel_counts,ResourceState::COPY_SRC,ResourceState::UNORDERED_ACCESS);device->Barrier(&copy,1,cmd);
    b[0]=GPUBarrier::Image(&s->reference_depth,ResourceState::SHADER_RESOURCE_COMPUTE,ResourceState::DEPTHSTENCIL);device->Barrier(b,1,cmd);s->pixels_pending=true;
}
inline void shutdown(){unbind_session();slots.clear();queue_shader={};pixel_shader={};}
}
