#pragma once
// Source-assisted economic experiment ONLY. Not ARC production admission.
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <cstring>
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"d3d12.lib")

namespace arc_controlled {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
inline double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
inline void check(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"offload HRESULT %08lx\n",(unsigned long)hr); std::fflush(stderr); ExitProcess(91);}}
struct State {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> upload,input,output,readback;
    ComPtr<ID3D12QueryHeap> query;
    ComPtr<ID3D12Resource> timestamps;
    HANDLE event{};
    uint64_t serial{},frequency{};
    uint32_t capacity{};
    std::mutex mutex;
    int mode{}; // 0 original, 1 CPU batch diagnostic, 2 GPU replacement, 3 GPU oracle, 4 GPU compaction, 5 compaction oracle
    ~State(){if(event)CloseHandle(event);}
    void attach(ID3D12Device* d){
        char value[32]{};GetEnvironmentVariableA("ARC_CONTROLLED_CULL",value,sizeof(value));
        mode=std::atoi(value); if(mode)device=d;
    }
    ComPtr<ID3D12Resource> buffer(uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
        D3D12_HEAP_PROPERTIES h{};h.Type=heap;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
        ComPtr<ID3D12Resource> result;check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)));return result;
    }
    void init(uint32_t count){
        if(!queue){
            auto start=Clock::now();
            const char* shader=R"(
cbuffer Params:register(b0){float4 planes[6];uint count;uint mask;uint compact;};
ByteAddressBuffer boxes:register(t0); RWStructuredBuffer<uint> result:register(u0);
groupshared uint localCount, base; groupshared uint ids[128];
[numthreads(128,1,1)] void main(uint3 tid:SV_DispatchThreadID,uint li:SV_GroupIndex){
uint i=tid.x;
if(compact){if(li==0)localCount=0;GroupMemoryBarrierWithGroupSync();}
bool visible=false;
if(i<count){
float3 lo=asfloat(boxes.Load3(i*32));uint layer=boxes.Load(i*32+12);
float3 hi=asfloat(boxes.Load3(i*32+16));
visible=(layer&mask)!=0 && !any(lo>hi);
[unroll]for(uint p=0;p<6;p++){
float4 pl=planes[p];float3 v=float3(pl.x<0?lo.x:hi.x,pl.y<0?lo.y:hi.y,pl.z<0?lo.z:hi.z);
// Release Wicked uses AVX/SSE4 dot: (x+y)+(z+w), no fused multiply-add.
precise float x=pl.x*v.x;precise float y=pl.y*v.y;precise float z=pl.z*v.z;
precise float a=x+y;precise float b=z+pl.w;precise float d=a+b;
if(d<0)visible=false;
}result[i]=visible?1:0;}
if(compact){
if(visible){uint slot;InterlockedAdd(localCount,1,slot);ids[slot]=i;}
GroupMemoryBarrierWithGroupSync();
if(li==0)InterlockedAdd(result[2*count],localCount,base);
GroupMemoryBarrierWithGroupSync();
if(li<localCount)result[count+base+li]=ids[li];
}})";
            ComPtr<ID3DBlob> code,error;
            auto hr=D3DCompile(shader,std::strlen(shader),"controlled-culling",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
            if(FAILED(hr)&&error)std::fprintf(stderr,"%s",(char*)error->GetBufferPointer());check(hr);
            D3D12_ROOT_PARAMETER params[3]{};
            params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=27;
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;
            ComPtr<ID3DBlob> blob;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
            check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
            D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
            check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
            check(device->CreateCommandList(0,q.Type,allocator.Get(),pso.Get(),IID_PPV_ARGS(&list)));check(list->Close());
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)ExitProcess(92);
            check(queue->GetTimestampFrequency(&frequency));
            D3D12_QUERY_HEAP_DESC qd{};qd.Count=2;qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;check(device->CreateQueryHeap(&qd,IID_PPV_ARGS(&query)));
            timestamps=buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            ARCWickedCpuSample("Offload startup",ms(start));
        }
        if(count>capacity){
            capacity=count;
            upload=buffer(uint64_t(count)*32+4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            input=buffer(uint64_t(count)*32,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            output=buffer(uint64_t(count*2+1)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            readback=buffer(uint64_t(count*2+1)*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    void transition(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);}
    int run(const wi::primitive::AABB* boxes,uint32_t n,const wi::primitive::Frustum& frustum,uint32_t mask,uint32_t* result){
        if(!mode||!n)return false;
        std::scoped_lock lock(mutex);
        auto total=Clock::now();
        if(mode==1){
            auto t=Clock::now();
            for(uint32_t i=0;i<n;++i)result[i]=(boxes[i].layerMask&mask)&&frustum.CheckBoxFast(boxes[i]);
            ARCWickedCpuSample("Offload CPU pure batch",ms(t));return true;
        }
        init(n);static_assert(sizeof(wi::primitive::AABB)==32);
        auto t=Clock::now();void* mapped{};D3D12_RANGE empty{0,0};
        check(upload->Map(0,&empty,&mapped));std::memcpy(mapped,boxes,size_t(n)*32);std::memset((char*)mapped+size_t(n)*32,0,4);upload->Unmap(0,nullptr);
        double pack=ms(t);t=Clock::now();
        check(allocator->Reset());check(list->Reset(allocator.Get(),pso.Get()));
        transition(input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(input.Get(),0,upload.Get(),0,uint64_t(n)*32);
        transition(input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootSignature(root.Get());list->SetComputeRoot32BitConstants(0,24,frustum.planes,0);list->SetComputeRoot32BitConstant(0,n,24);list->SetComputeRoot32BitConstant(0,mask,25);list->SetComputeRoot32BitConstant(0,mode>=4?1:0,26);
        if(mode>=4){transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(output.Get(),uint64_t(n)*8,upload.Get(),uint64_t(n)*32,4);transition(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        list->SetComputeRootShaderResourceView(1,input->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
        list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);list->Dispatch((n+127)/128,1,1);list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,uint64_t(mode>=4?n*2+1:n)*4);transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamps.Get(),0);check(list->Close());
        ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);check(queue->Signal(fence.Get(),++serial));
        double submit=ms(t);t=Clock::now();
        if(fence->GetCompletedValue()<serial){check(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)ExitProcess(93);}
        double wait=ms(t);t=Clock::now();const size_t resultBytes=size_t(mode>=4?n*2+1:n)*4;D3D12_RANGE range{0,resultBytes};check(readback->Map(0,&range,&mapped));std::memcpy(result,mapped,resultBytes);readback->Unmap(0,&empty);
        double commit=ms(t);D3D12_RANGE qr{0,16};check(timestamps->Map(0,&qr,&mapped));auto* stamps=(uint64_t*)mapped;double kernel=double(stamps[1]-stamps[0])*1000/double(frequency);timestamps->Unmap(0,&empty);
        double complete=ms(total);
        if(mode==3||mode==5){uint32_t bad=0;for(uint32_t i=0;i<n;++i)if(result[i]!=uint32_t((boxes[i].layerMask&mask)&&frustum.CheckBoxFast(boxes[i])))++bad;ARCWickedCpuSample("Offload mismatches count",bad);if(mode==5){std::vector<uint8_t> seen(n);uint32_t expected=0;for(uint32_t i=0;i<n;++i)expected+=result[i]!=0;if(result[2*n]!=expected)++bad;for(uint32_t i=0;i<result[2*n]&&i<n;++i){auto id=result[n+i];if(id>=n||!result[id]||seen[id])++bad;else seen[id]=1;}ARCWickedCpuSample("Offload compact errors count",bad);}if(bad)ExitProcess(94);}
        ARCWickedCpuSample("Offload total",complete);ARCWickedCpuSample("Offload pack",pack);ARCWickedCpuSample("Offload submit",submit);ARCWickedCpuSample("Offload wait",wait);ARCWickedCpuSample("Offload commit",commit);ARCWickedCpuSample("Offload kernel",kernel);ARCWickedCpuSample("Offload predicates count",n);
        return mode>=4?2:1;
    }
};
inline State state;
}
extern "C" int ARCControlledCull(const void* boxes,uint32_t n,const void* frustum,uint32_t mask,uint32_t* result){return arc_controlled::state.run(static_cast<const wi::primitive::AABB*>(boxes),n,*static_cast<const wi::primitive::Frustum*>(frustum),mask,result);}
