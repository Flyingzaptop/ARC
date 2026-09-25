#pragma once
// Manual Wicked adapter for an economic experiment, not an ARC admission rule.
#include "wiScene.h"
#include "wiGraphicsDevice_DX12.h"
#include "ArcWickedHooks.h"
#include "pack_gpu.hpp"
#include <atomic>
#include <mutex>
namespace arc_packet_fixture {
using namespace wi::graphics;
struct Extra {float transparency;uint32_t lod,alpha;};
inline int mode(){static int v=[](){char s[16]{};GetEnvironmentVariableA("ARC_PACKET_MODE",s,16);return atoi(s);}();return v;}
inline std::vector<Extra> side;
inline const wi::scene::Scene* source{};
inline uint64_t prepared=~0ull;
inline arc_packet::Worker uploader;
inline Microsoft::WRL::ComPtr<ID3D12Resource> sideUpload;
inline void* sideMapped{};
inline bool active{};
inline Extra* producer(const wi::scene::Scene* scene,uint32_t n){if(!mode()||n>131072)return nullptr;source=scene;side.resize(n);return side.data();}
inline bool moved(const wi::scene::Scene* scene){return active&&source==scene&&prepared==GetDevice()->GetFrameCount();}
inline void publish();
inline void prepare(const wi::scene::Scene* scene){
 publish();active=false;if(mode()==0||mode()==3||source!=scene||side.empty()||!scene->instanceBuffer.IsValid())return;
 auto* d=static_cast<GraphicsDevice_DX12*>(GetDevice());if(!d)return;auto t=arc_packet::Clock::now();
 if(!uploader.device){uploader.init(d->ArcPacketDevice(),d->ArcPacketQueue(),L"C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/record-pack-gpu/pack.hlsl");sideUpload=uploader.buffer(131072*12,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);D3D12_RANGE e{};arc_packet::check(sideUpload->Map(0,&e,&sideMapped));}
 // Only our previous upload, not a global GPU drain. Normally already completed by last packet.
 if(uploader.serial&&uploader.fence->GetCompletedValue()<uploader.serial){arc_packet::check(uploader.fence->SetEventOnCompletion(uploader.serial,uploader.event));if(WaitForSingleObject(uploader.event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("sidecar reuse timeout");}
 memcpy(sideMapped,side.data(),side.size()*12);arc_packet::check(uploader.allocator->Reset());arc_packet::check(uploader.list->Reset(uploader.allocator.Get(),nullptr));
 auto* dst=d->ArcPacketResource(&scene->instanceBuffer);auto* src=d->ArcPacketResource(&scene->instanceUploadBuffer[scene->cpu_gpu_mapped_resource_index]);
 uploader.barrier(dst,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);uploader.list->CopyBufferRegion(dst,0,src,0,scene->instanceArraySize*sizeof(ShaderMeshInstance));uploader.barrier(dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);arc_packet::check(uploader.list->Close());ID3D12CommandList* c=uploader.list.Get();uploader.queue->ExecuteCommandLists(1,&c);arc_packet::check(uploader.queue->Signal(uploader.fence.Get(),++uploader.serial));
 prepared=GetDevice()->GetFrameCount();active=true;ARCWickedCpuSample("Packet early upload CPU ms",arc_packet::elapsed(t));ARCWickedCpuSample("Packet sidecar bytes",double(side.size()*12));
}
struct Slot {arc_packet::Worker worker;GPUBuffer output;arc_packet::Result result;std::atomic<uint64_t> claimed{~0ull};arc_packet::Metrics metrics{};uint32_t records{},validated{},replaced{};double retained{},cpuLoop{},cpuFlush{};bool pending{};};
inline Slot slots[3][2];
inline Slot* run(const wi::scene::Scene* scene,const void* records,uint32_t n,int pass){
 if(!moved(scene)||n==0||n>65536||pass<0||pass>1)return nullptr;auto* d=static_cast<GraphicsDevice_DX12*>(GetDevice());if(!d)return nullptr;uint64_t frame=d->GetFrameCount();auto& s=slots[frame%3][pass];if(s.claimed.exchange(frame)==frame)return nullptr;
 if(!s.worker.device){s.worker.init(d->ArcPacketDevice(),d->ArcPacketQueue(),L"C:/Users/r3d_flzp/Desktop/ARC-perceptual/experiments/record-pack-gpu/pack.hlsl");GPUBufferDesc desc;desc.size=65536*64;desc.stride=4;desc.bind_flags=BindFlag::SHADER_RESOURCE|BindFlag::UNORDERED_ACCESS;desc.misc_flags=ResourceMiscFlag::BUFFER_RAW;if(!d->CreateBuffer(&desc,nullptr,&s.output))throw std::runtime_error("packet output");}
 arc_packet::Metrics m;s.result=s.worker.run(records,n,uint32_t(side.size()),true,d->ArcPacketResource(&scene->instanceBuffer),sideUpload.Get(),d->ArcPacketResource(&s.output),m,mode()==1);
 s.metrics=m;s.records=n;s.pending=true;
 if(s.result.error||s.result.count>256){return nullptr;}return &s;
}
inline void cpuCost(int pass,double loop,double flush){auto& s=slots[GetDevice()->GetFrameCount()%3][pass];s.cpuLoop=loop;s.cpuFlush=flush;s.pending=true;}
struct FlushTimer{double& total;arc_packet::Clock::time_point start=arc_packet::Clock::now();~FlushTimer(){total+=arc_packet::elapsed(start);}};
inline void publish(){
 for(auto& frame:slots)for(auto& s:frame){if(!s.pending)continue;s.pending=false;
 ARCWickedCpuSample("Original pack loop ms",s.cpuLoop);ARCWickedCpuSample("Original pack flush ms",s.cpuFlush);ARCWickedCpuSample("Packet full ms",s.metrics.full);ARCWickedCpuSample("Packet copy ms",s.metrics.copy);ARCWickedCpuSample("Packet recording ms",s.metrics.record);ARCWickedCpuSample("Packet wait ms",s.metrics.wait);ARCWickedCpuSample("Packet GPU compute ms",s.metrics.gpu);ARCWickedCpuSample("Packet GPU record upload ms",s.metrics.gpu_upload);ARCWickedCpuSample("Packet GPU metadata return ms",s.metrics.gpu_return);ARCWickedCpuSample("Packet groups",s.result.count);ARCWickedCpuSample("Packet retained consumer ms",s.retained);ARCWickedCpuSample("Packet validated records",s.validated);ARCWickedCpuSample("Packet replaced records",s.replaced);ARCWickedCpuSample("Packet fallback",s.result.error||s.result.count>256);}
}

}
