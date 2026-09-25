#pragma once
// Bounded research backend: 65536 records, 256 CPU-visible groups, 16 cameras.
// Caller retains inputs/output and guarantees queue ordering and resource states.
#include <windows.h>
#ifndef __ID3D12Device_INTERFACE_DEFINED__
#include <d3d12.h>
#endif
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <array>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <string>
namespace arc_packet {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
inline double elapsed(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
inline void check(HRESULT h){if(FAILED(h))throw std::runtime_error("packet D3D12 failure: "+std::to_string(unsigned(h)));}
struct Group {uint32_t mesh,lod,stencil,alpha,offset,count,first,end;};
struct Metrics {double full{},copy{},record{},submit{},wait{},consume{},gpu{},gpu_upload{},gpu_return{};};
struct Result {uint32_t count{},words{},error{},reserved{};Group groups[256]{};};
static_assert(sizeof(Result)==8208);
struct Worker {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
 ComPtr<ID3D12Fence> fence;HANDLE event{};uint64_t serial{},frequency{};
 ComPtr<ID3D12RootSignature> root;std::array<ComPtr<ID3D12PipelineState>,6> shaders;
 ComPtr<ID3D12Resource> upload,records,work,prefix,blocks,meta,readback,timeback,verify,templateUpload,argReadback;
 ComPtr<ID3D12QueryHeap> query;char* templateMapped{};char* mapped{};char* returned{};uint64_t* times{};
 static constexpr uint32_t capacity=65536;
 ComPtr<ID3D12Resource> buffer(uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,bool uav=false){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=bytes;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;r.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;ComPtr<ID3D12Resource> b;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&b)));return b;
 }
 void init(ID3D12Device* d,ID3D12CommandQueue* q,const wchar_t* path){
  device=d;queue=q;check(q->GetTimestampFrequency(&frequency));check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("packet event");
  D3D12_ROOT_PARAMETER p[11]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants={0,0,4};
  for(int i=1;i<11;++i){p[i].ParameterType=i<5?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i<5?i-1:i-5;}
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=11;rd.pParameters=p;ComPtr<ID3DBlob> b,e;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&b,&e));check(d->CreateRootSignature(0,b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&root)));
  const char* entries[]={"evaluate","scanblocks","groups","scatter","finish","drawargs"};
  for(int i=0;i<6;++i){ComPtr<ID3DBlob> code;auto h=D3DCompileFromFile(path,nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entries[i],"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&e);if(FAILED(h))throw std::runtime_error(e?std::string((char*)e->GetBufferPointer(),e->GetBufferSize()):"compile");D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&shaders[i])));}
  upload=buffer(capacity*16+16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);records=buffer(capacity*16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  work=buffer(capacity*16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);prefix=buffer(capacity*8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);blocks=buffer(256*8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);meta=buffer(sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);readback=buffer(sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);timeback=buffer(32,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  templateUpload=buffer(64*20,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_RANGE empty{};check(templateUpload->Map(0,&empty,(void**)&templateMapped));check(upload->Map(0,&empty,(void**)&mapped));check(readback->Map(0,nullptr,(void**)&returned));check(timeback->Map(0,nullptr,(void**)&times));memset(mapped+capacity*16,0,16);D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=4;check(d->CreateQueryHeap(&hd,IID_PPV_ARGS(&query)));
 }
 ~Worker(){if(event)CloseHandle(event);}
 void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);}
 void uav(){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);}
 void wait(){check(queue->Signal(fence.Get(),++serial));if(fence->GetCompletedValue()<serial){check(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("packet fence timeout");}check(device->GetDeviceRemovedReason());}
 void run(const void* input,uint32_t n,uint32_t objectCount,bool stencil,ID3D12Resource* objects,ID3D12Resource* sidecar,ID3D12Resource* output,ID3D12Resource* args,const uint32_t* templates,uint32_t drawCount,Metrics& m,bool validate=false){
  if(n==0||n>capacity||drawCount>64||drawCount==0)throw std::runtime_error("packet record budget");auto full=Clock::now(),t=full;memcpy(mapped,input,size_t(n)*16);memcpy(templateMapped,templates,drawCount*20);m.copy=elapsed(t);t=Clock::now();check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
  auto stamp=[&](int i){list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i);};stamp(0);
  barrier(records.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(records.Get(),0,upload.Get(),0,n*16);barrier(records.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  barrier(meta.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(meta.Get(),0,upload.Get(),capacity*16,16);barrier(meta.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barrier(output,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);barrier(args,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);stamp(1);list->SetComputeRootSignature(root.Get());uint32_t p[]={n,uint32_t(stencil),objectCount,drawCount};list->SetComputeRoot32BitConstants(0,4,p,0);
  ID3D12Resource* srv[]={records.Get(),objects,sidecar,templateUpload.Get()};for(int i=0;i<4;++i)list->SetComputeRootShaderResourceView(i+1,srv[i]->GetGPUVirtualAddress());ID3D12Resource* out[]={work.Get(),prefix.Get(),blocks.Get(),meta.Get(),output,args};for(int i=0;i<6;++i)list->SetComputeRootUnorderedAccessView(i+5,out[i]->GetGPUVirtualAddress());
  for(int i=0;i<6;++i){list->SetPipelineState(shaders[i].Get());list->Dispatch(i==1||i==4||i==5?1:(n+255)/256,1,1);uav();}stamp(2);
  if(validate){barrier(meta.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(readback.Get(),0,meta.Get(),0,sizeof(Result));barrier(meta.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}stamp(3);
  if(validate){if(!verify)verify=buffer(capacity*64,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);barrier(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(verify.Get(),0,output,0,n*64);barrier(output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);}else barrier(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  if(validate){if(!argReadback)argReadback=buffer(64*20,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);barrier(args,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(argReadback.Get(),0,args,0,drawCount*20);barrier(args,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);}else barrier(args,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,4,timeback.Get(),0);check(list->Close());m.record=elapsed(t);t=Clock::now();ID3D12CommandList* c=list.Get();queue->ExecuteCommandLists(1,&c);check(queue->Signal(fence.Get(),++serial));m.submit=elapsed(t);m.full=elapsed(full);m.wait=0;
 } 
 void completedMetrics(Metrics& m){m.gpu=double(times[2]-times[1])*1000/frequency;m.gpu_upload=double(times[1]-times[0])*1000/frequency;m.gpu_return=double(times[3]-times[2])*1000/frequency;
 }
};
}
