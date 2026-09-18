#include "arc/gpu_attribution.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
using Microsoft::WRL::ComPtr;
void hr(HRESULT v){if(FAILED(v))throw std::runtime_error("D3D12 failure: "+std::to_string(v));}
int main(int argc,char** argv)try{
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
    for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i){
        DXGI_ADAPTER_DESC1 desc;hr(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE){adapter.Reset();continue;}
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))break;adapter.Reset();
    }
    if(!device){std::cerr<<"No native D3D12 hardware\n";return 77;}
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    constexpr UINT64 bytes=4*1024*1024;
    auto buffer=[&](D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,UINT64 size){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=size;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,bytes);
    auto a=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,bytes);
    auto b=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,bytes);
    auto readback=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,bytes);
    auto times=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,6*sizeof(UINT64));
    void* data{};D3D12_RANGE empty{0,0};hr(upload->Map(0,&empty,&data));
    for(UINT64 i=0;i<bytes/4;++i)static_cast<UINT*>(data)[i]=static_cast<UINT>(i*2654435761u);upload->Unmap(0,nullptr);
    ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=6;
    hr(device->CreateQueryHeap(&hd,IID_PPV_ARGS(&queries)));
    arc::GpuAttributionGraph graph;graph.begin(1);
    auto copy=[&](ID3D12Resource* src,ID3D12Resource* dst,UINT index,arc::ResourceId source,arc::ResourceId destination){
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2);
        list->CopyBufferRegion(dst,0,src,0,bytes);
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2+1);
        arc::WorkObservation w;w.kind=arc::GpuWorkKind::Copy;w.bindings_complete=true;w.copy_bytes=bytes;
        w.accesses={{source,false,arc::AccessEvidence::Observed,false},{destination,true,arc::AccessEvidence::Observed,true}};
        if(!graph.record(1,w))throw std::runtime_error("record failed");
    };
    auto transition=[&](ID3D12Resource* r){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);};
    copy(upload.Get(),a.Get(),0,1,2);transition(a.Get());copy(a.Get(),b.Get(),1,2,3);transition(b.Get());copy(b.Get(),readback.Get(),2,3,4);
    list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,6,times.Get(),0);hr(list->Close());
    ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);graph.close(1);auto ids=graph.submit(1,1);
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("event");
    hr(queue->Signal(fence.Get(),1));hr(fence->SetEventOnCompletion(1,event));auto wait=WaitForSingleObject(event,30000);CloseHandle(event);
    if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout");
    hr(readback->Map(0,nullptr,&data));bool valid=true;for(UINT64 i=0;i<bytes/4;++i)if(static_cast<UINT*>(data)[i]!=static_cast<UINT>(i*2654435761u)){valid=false;break;}readback->Unmap(0,&empty);
    UINT64 frequency{};hr(queue->GetTimestampFrequency(&frequency));hr(times->Map(0,nullptr,&data));
    auto* ticks=static_cast<UINT64*>(data);std::array<UINT64,6> raw{};std::copy(ticks,ticks+6,raw.begin());times->Unmap(0,&empty);
    if(ids.size()!=3||graph.edges().size()!=2)throw std::runtime_error("wrong topology");
    for(UINT i=0;i<3;++i)valid=graph.timing(ids[i],{raw[2*i],raw[2*i+1],frequency,true})&&valid;
    if(!valid||!graph.complete())throw std::runtime_error("readback or timestamps failed");
    std::ofstream out(argc>1?argv[1]:"mega-d-native.json");if(!out)throw std::runtime_error("output");
    out<<"{\"schema\":1,\"hardware\":true,\"readback_verified\":true,\"frequency\":"<<frequency<<",\"timestamp_ticks\":[";
    for(int i=0;i<6;++i){if(i)out<<',';out<<raw[i];}out<<"],\"graph\":";graph.write_json(out);out<<"}\n";
    std::cout<<"Native GPU copy chain: 4 MiB readback verified; 3 timestamp pairs; 2 dependency edges PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
