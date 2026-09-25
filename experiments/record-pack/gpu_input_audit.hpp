#pragma once
// Read-only, source-assisted two-frame audit. Not a production ownership proof.
#include "wiScene.h"
#include "wiGraphicsDevice.h"
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstddef>
namespace arc_pack_gpu_audit {
using namespace wi::graphics;
struct Expected {float center[3],fade,radius,alpha,alpha_ref;uint32_t lod,stencil;};
static_assert(sizeof(Expected)==36);
inline std::mutex mutex;
inline GPUBuffer readback,held_source;
inline std::vector<Expected> expected,previous;
inline uint64_t frame{};inline uint32_t objects{},instances{},ordinal{};
inline bool pending{};
inline const char* destination(){return std::getenv("ARC_PACK_GPU_AUDIT");}
inline void save(const std::string& path,const void* bytes,size_t size){FILE* f{};fopen_s(&f,path.c_str(),"wb");if(!f)std::abort();std::fwrite(bytes,1,size,f);std::fclose(f);}
inline void enqueue(const wi::scene::Scene& scene,CommandList cmd){
 if(!destination())return;
 std::lock_guard<std::mutex> lock(mutex);
 if(pending||ordinal>=2||scene.objects.GetCount()<1024)return;
 auto* device=GetDevice();objects=uint32_t(scene.objects.GetCount());instances=uint32_t(scene.instanceArraySize);
 uint64_t bytes=uint64_t(instances)*sizeof(ShaderMeshInstance);
 if(bytes>32*1024*1024||objects>instances)std::abort();
 GPUBufferDesc desc;desc.usage=Usage::READBACK;desc.size=bytes;
 if(!device->CreateBuffer(&desc,nullptr,&readback))std::abort();
 expected.resize(objects);
 for(uint32_t i=0;i<objects;++i){auto& o=scene.objects[i];auto& e=expected[i];std::memcpy(e.center,&o.center,12);e.fade=o.fadeDistance;e.radius=o.radius;e.alpha=o.color.w;e.alpha_ref=o.alphaRef;e.lod=o.lod;e.stencil=o.userStencilRef;}
 held_source=scene.instanceBuffer;frame=device->GetFrameCount();
 auto b=GPUBarrier::Buffer(&held_source,ResourceState::COPY_DST,ResourceState::COPY_SRC);device->Barrier(&b,1,cmd);
 device->CopyBuffer(&readback,0,&held_source,0,bytes,cmd);
 b=GPUBarrier::Buffer(&held_source,ResourceState::COPY_SRC,ResourceState::COPY_DST);device->Barrier(&b,1,cmd);
 pending=true;
}
inline void poll(){
 if(!destination())return;
 std::lock_guard<std::mutex> lock(mutex);
 auto* device=GetDevice();if(!pending||device->GetFrameCount()<=frame)return;
 // Prior-frame command lists have been submitted. Completion precedes readback.
 device->WaitForGPU();
 auto* gpu=static_cast<const ShaderMeshInstance*>(readback.mapped_data);
 uint64_t exact_errors=0,alpha_full_errors=0,alpha_test_full_errors=0,changed_centers=0;
 for(uint32_t i=0;i<objects;++i){auto& g=gpu[i];auto& e=expected[i];
  exact_errors+=std::memcmp(&g.center,e.center,12)!=0||std::memcmp(&g.fadeDistance,&e.fade,4)!=0||std::memcmp(&g.radius,&e.radius,4)!=0||(g.flags>>24)!=e.stencil;
  float alpha=DirectX::PackedVector::XMConvertHalfToFloat(uint16_t(g.color.y>>16));
  float alpha_ref=1-DirectX::PackedVector::XMConvertHalfToFloat(uint16_t(g.alphaTest_size));
  alpha_full_errors+=alpha!=e.alpha;alpha_test_full_errors+=alpha_ref!=e.alpha_ref;
  if(previous.size()==expected.size())changed_centers+=std::memcmp(previous[i].center,e.center,12)!=0;
 }
 std::string base=std::string(destination())+"."+std::to_string(ordinal);
 save(base+".expected.bin",expected.data(),expected.size()*sizeof(Expected));
 save(base+".gpu.bin",gpu,uint64_t(instances)*sizeof(ShaderMeshInstance));
 FILE* f{};fopen_s(&f,(base+".json").c_str(),"wb");if(!f)std::abort();
 std::fprintf(f,"{\"frame\":%llu,\"read_after_frame\":%llu,\"wait_for_gpu\":true,\"objects\":%u,\"instances\":%u,\"stride\":%zu,\"center_offset\":%zu,\"fade_offset\":%zu,\"radius_offset\":%zu,\"color_offset\":%zu,\"alpha_test_offset\":%zu,\"flags_offset\":%zu,\"exact_errors\":%llu,\"alpha_full_errors\":%llu,\"alpha_test_full_errors\":%llu,\"changed_centers\":%llu,\"source_identity\":%llu}\n",frame,device->GetFrameCount(),objects,instances,sizeof(ShaderMeshInstance),offsetof(ShaderMeshInstance,center),offsetof(ShaderMeshInstance,fadeDistance),offsetof(ShaderMeshInstance,radius),offsetof(ShaderMeshInstance,color),offsetof(ShaderMeshInstance,alphaTest_size),offsetof(ShaderMeshInstance,flags),exact_errors,alpha_full_errors,alpha_test_full_errors,changed_centers,uint64_t(reinterpret_cast<uintptr_t>(held_source.internal_state.get())));
 std::fclose(f);previous=expected;pending=false;++ordinal;
}
}
