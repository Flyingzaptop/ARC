#pragma once
#include "wiScene.h"
#include "wiGraphicsDevice_DX12.h"
#include "ArcWickedHooks.h"
#include "pack_gpu.hpp"
#include "admission.hpp"
#include <atomic>
#include <cmath>
#include <limits>
namespace arc_indirect {
using namespace wi::graphics;
using namespace arc_packet;
extern "C" void ARCWickedFrameSample(const char*,double,uint64_t) noexcept;
inline int mode(){static int m=[](){char b[16]{};GetEnvironmentVariableA("ARC_INDIRECT_MODE",b,16);return atoi(b);}();return m;}
using Extra=arc_indirect_contract::Extra;
static_assert(sizeof(Extra)==20);
inline std::vector<Extra> side;inline const wi::scene::Scene* source{};
inline Extra* producer(const wi::scene::Scene* scene,uint32_t n){if(!mode()||n>131072)return nullptr;source=scene;side.resize(n);return side.data();}
inline Extra makeExtra(const wi::scene::ObjectComponent& o){bool ok=o.color.w==1 && o.alphaRef>=1 && o.fadeDistance==std::numeric_limits<float>::max() && std::isfinite(o.radius)&&o.radius>0;return {o.GetTransparency(),o.lod,uint32_t(o.alphaRef<1),uint32_t(o.userStencilRef)|(ok?256:0),uint32_t(GetDevice()->GetFrameCount())};}
inline ComPtr<ID3D12Fence> consumers;
using arc_indirect_contract::reusable;
struct Frame{
 uint64_t frame=~0ull;bool reported=true,uploaded=false;
 ComPtr<ID3D12CommandAllocator> a,b;ComPtr<ID3D12GraphicsCommandList> start,end;
 ComPtr<ID3D12QueryHeap> query;ComPtr<ID3D12Resource> times,extra;uint64_t* stamps{};void* extraMapped{};uint64_t frequency{};
 void init(GraphicsDevice_DX12* d){if(a)return;auto* dev=d->ArcPacketDevice();if(!consumers)check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumers)));check(d->ArcPacketQueue()->GetTimestampFrequency(&frequency));check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&b)));check(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&start)));check(start->Close());check(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,b.Get(),nullptr,IID_PPV_ARGS(&end)));check(end->Close());D3D12_QUERY_HEAP_DESC h{};h.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;h.Count=2;check(dev->CreateQueryHeap(&h,IID_PPV_ARGS(&query)));Worker helper;helper.device=dev;times=helper.buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);extra=helper.buffer(131072*20,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);D3D12_RANGE empty{};check(extra->Map(0,&empty,&extraMapped));check(times->Map(0,nullptr,(void**)&stamps));}
};
struct Slot{Worker worker;GPUBuffer output,args;uint64_t frame=~0ull;std::atomic<uint64_t> claim{~0ull};Metrics metrics{};Group group{};bool pending{};double preflight{},retained{};uint32_t records{},drawCount{},commands{};std::vector<uint32_t> expected,templates;};
inline Frame frames[3];inline Slot slots[3][2];inline Frame* current{};
inline void poll(){if(!consumers)return;auto done=consumers->GetCompletedValue();
 for(auto& f:frames){if(!f.reported&&reusable(f.frame,done)){f.reported=true;ARCWickedFrameSample("Indirect complete GPU envelope ms",double(f.stamps[1]-f.stamps[0])*1000/f.frequency,f.frame);}}
 for(auto& pair:slots)for(auto& s:pair){if(!s.pending||!reusable(s.frame,done))continue;s.pending=false;s.worker.completedMetrics(s.metrics);
  if(mode()==2){Result got;memcpy(&got,s.worker.returned,sizeof(got));Group expected=s.group;expected.count=uint32_t(s.expected.size());expected.first=0;expected.end=s.records;
   if(got.error||got.count!=1||got.words!=s.expected.size()||memcmp(&got.groups[0],&expected,sizeof(Group)))throw std::runtime_error("indirect metadata oracle");
   void* p{};check(s.worker.verify->Map(0,nullptr,&p));bool equal=memcmp(p,s.expected.data(),s.expected.size()*4)==0;s.worker.verify->Unmap(0,nullptr);if(!equal)throw std::runtime_error("indirect words oracle");
   check(s.worker.argReadback->Map(0,nullptr,&p));auto* args=(uint32_t*)p;for(uint32_t i=0;i<s.drawCount;++i){uint32_t ref[5]={s.templates[i*5],uint32_t(s.expected.size()),s.templates[i*5+2],0,0};if(memcmp(args+i*5,ref,20))throw std::runtime_error("indirect command oracle");}s.worker.argReadback->Unmap(0,nullptr);ARCWickedFrameSample("Indirect validated words",double(s.expected.size()),s.frame);ARCWickedFrameSample("Indirect validated commands",s.commands,s.frame);
  }
  auto sample=[&](const char* n,double v){ARCWickedFrameSample(n,v,s.frame);};sample("Indirect preflight ms",s.preflight);sample("Indirect packet CPU ms",s.metrics.full);sample("Indirect CPU metadata wait ms",s.metrics.wait);sample("Indirect GPU work ms",s.metrics.gpu);sample("Indirect GPU input copy ms",s.metrics.gpu_upload);sample("Indirect retained CPU ms",s.retained);sample("Indirect records replaced",s.records);sample("Indirect commands",s.commands);
 }
}
inline void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&v);}
inline void prepare(const wi::scene::Scene* scene){poll();current=nullptr;auto* d=static_cast<GraphicsDevice_DX12*>(GetDevice());auto frame=d->GetFrameCount();auto& f=frames[frame%3];f.init(d);if(!reusable(f.frame,consumers->GetCompletedValue())){ARCWickedCpuSample("Indirect busy frame fallback",1);return;}auto t=Clock::now();f.frame=frame;f.reported=false;f.uploaded=mode()!=0&&source==scene&&!side.empty()&&scene->instanceBuffer.IsValid();check(f.a->Reset());check(f.start->Reset(f.a.Get(),nullptr));f.start->EndQuery(f.query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
 if(f.uploaded){memcpy(f.extraMapped,side.data(),side.size()*sizeof(Extra));auto* dst=d->ArcPacketResource(&scene->instanceBuffer);auto* src=d->ArcPacketResource(&scene->instanceUploadBuffer[scene->cpu_gpu_mapped_resource_index]);transition(f.start.Get(),dst,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);f.start->CopyBufferRegion(dst,0,src,0,scene->instanceArraySize*sizeof(ShaderMeshInstance));transition(f.start.Get(),dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);}
 check(f.start->Close());ID3D12CommandList* c=f.start.Get();d->ArcPacketQueue()->ExecuteCommandLists(1,&c);current=&f;ARCWickedCpuSample("Indirect frame preparation CPU ms",elapsed(t));}
inline bool moved(const wi::scene::Scene* s){return current&&current->uploaded&&source==s&&current->frame==GetDevice()->GetFrameCount();}
inline void endFrame(ID3D12CommandQueue* q,uint64_t frame){if(!consumers)return;auto& f=frames[frame%3];if(f.frame==frame){check(f.b->Reset());check(f.end->Reset(f.b.Get(),nullptr));f.end->EndQuery(f.query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);f.end->ResolveQueryData(f.query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,f.times.Get(),0);check(f.end->Close());ID3D12CommandList* c=f.end.Get();q->ExecuteCommandLists(1,&c);}check(q->Signal(consumers.Get(),frame+1));}
inline Slot* preflight(const wi::scene::Scene* scene,const void* input,uint32_t n,int pass){
 if(!moved(scene)||n==0||n>65536||pass<0||pass>1)return nullptr;auto t=Clock::now();auto frame=GetDevice()->GetFrameCount();auto& s=slots[frame%3][pass];if(s.claim.exchange(frame)==frame||!reusable(s.frame,consumers->GetCompletedValue()))return nullptr;arc_indirect_contract::Group key{};
 if(!arc_indirect_contract::admit((const uint32_t*)input,n,side.data(),side.size(),uint32_t(frame),key))return nullptr;
 Group group{key.mesh,key.lod,key.stencil,0,0,0,0,n};
 s.group=group;s.preflight=elapsed(t);s.records=n;s.frame=frame;s.commands=0;return &s;
}
inline void submit(Slot& s,const wi::scene::Scene* scene,const void* records,const std::vector<uint32_t>& templates){auto* d=static_cast<GraphicsDevice_DX12*>(GetDevice());
 if(!s.worker.device){s.worker.init(d->ArcPacketDevice(),d->ArcPacketQueue(),L"C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/record-pack-indirect/pack.hlsl");GPUBufferDesc desc;desc.size=65536*64;desc.bind_flags=BindFlag::SHADER_RESOURCE|BindFlag::UNORDERED_ACCESS;desc.misc_flags=ResourceMiscFlag::BUFFER_RAW;if(!d->CreateBuffer(&desc,nullptr,&s.output))throw std::runtime_error("output allocation");desc.size=64*20;desc.misc_flags=ResourceMiscFlag::BUFFER_RAW|ResourceMiscFlag::INDIRECT_ARGS;if(!d->CreateBuffer(&desc,nullptr,&s.args))throw std::runtime_error("args allocation");}
 s.templates=templates;s.drawCount=uint32_t(templates.size()/5);s.worker.run(records,s.records,uint32_t(side.size()),true,d->ArcPacketResource(&scene->instanceBuffer),current->extra.Get(),d->ArcPacketResource(&s.output),d->ArcPacketResource(&s.args),templates.data(),s.drawCount,s.metrics,mode()==2);s.pending=true;
}
}
