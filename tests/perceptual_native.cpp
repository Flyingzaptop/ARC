#include "arc/perceptual_trial.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("D3D12 HRESULT "+std::to_string(r));}
class NativeProbe final:public arc::PerceptualProbeHost {
    static constexpr UINT side=128, texture_side=4096, mip_count=4, repeats=11;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> texture,output,pixels,times;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
    UINT64 fence_value{},frequency{};
    UINT descriptor_stride{};
    HANDLE event{};
    unsigned mip{},trial_index{};
    std::filesystem::path directory;
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)));return result;
    }
    void transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
    }
    void execute(){
        hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        hr(queue->Signal(fence.Get(),++fence_value));hr(fence->SetEventOnCompletion(fence_value,event));
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");
        hr(device->GetDeviceRemovedReason());
    }
    void reset(){hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));}
    void set_mip(unsigned value){
        if(fence->GetCompletedValue()<fence_value)throw std::runtime_error("descriptor still in flight");
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MostDetailedMip=value;view.Texture2D.MipLevels=mip_count-value;
        device->CreateShaderResourceView(texture.Get(),&view,heap->GetCPUDescriptorHandleForHeapStart());mip=value;
    }
public:
    explicit NativeProbe(std::filesystem::path path):directory(std::move(path)){
        std::filesystem::create_directories(directory);
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;auto result=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));if(result==DXGI_ERROR_NOT_FOUND)break;hr(result);
            DXGI_ADAPTER_DESC1 desc{};hr(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))break;}
        if(!device)throw std::runtime_error("No hardware D3D12 adapter");
        D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));hr(queue->GetTimestampFrequency(&frequency));
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("event");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));descriptor_stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=texture_side;td.Height=texture_side;td.DepthOrArraySize=1;td.MipLevels=mip_count;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,mip_count> footprints{};UINT64 upload_bytes{};
        device->GetCopyableFootprints(&td,0,mip_count,0,footprints.data(),nullptr,nullptr,&upload_bytes);
        auto upload=buffer(upload_bytes,D3D12_HEAP_TYPE_UPLOAD);void* data{};D3D12_RANGE empty{0,0};hr(upload->Map(0,&empty,&data));
        for(UINT level=0;level<mip_count;++level){const auto& f=footprints[level];
            for(UINT y=0;y<f.Footprint.Height;++y){auto* row=static_cast<unsigned char*>(data)+f.Offset+UINT64(y)*f.Footprint.RowPitch;
                for(UINT x=0;x<f.Footprint.Width;++x){row[x*4]=level==3?255:static_cast<unsigned char>(96+32*x/f.Footprint.Width);row[x*4+1]=level==3?0:static_cast<unsigned char>(96+32*y/f.Footprint.Height);row[x*4+2]=level==3?0:112;row[x*4+3]=255;}}
        }
        upload->Unmap(0,nullptr);
        for(UINT level=0;level<mip_count;++level){D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[level];D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=level;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
        transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        td.Width=side;td.Height=side;td.MipLevels=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
        UINT64 bytes{};device->GetCopyableFootprints(&td,0,1,0,&output_footprint,nullptr,nullptr,&bytes);pixels=buffer(bytes,D3D12_HEAP_TYPE_READBACK);times=buffer(repeats*2*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=td.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;auto handle=heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=descriptor_stride;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,handle);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={2,ranges};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> blob,errors;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        const char* shader=R"(
Texture2D<float4> source:register(t0); RWTexture2D<float4> output_image:register(u0); SamplerState linear_sampler:register(s0);
[numthreads(8,8,1)] void main(uint3 tid:SV_DispatchThreadID) {
 float2 uv=(float2(tid.xy)+.5)/128; float3 sum=0;
 [loop] for(uint i=0;i<128;++i) {
  float2 address=frac(uv+float2(i*.6180339,i*.4142135));
  sum+=source.SampleLevel(linear_sampler,address,0).rgb;
 }
 // Only this small output region depends on the costly sampled work.
 output_image[tid.xy]=float4(tid.x<16&&tid.y<16?sum/128:float3(.3,.3,.3),1);
})";
        hr(D3DCompile(shader,std::strlen(shader),"portable-probe",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
        D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=repeats*2;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));
        execute();set_mip(0);
    }
    ~NativeProbe(){if(event)CloseHandle(event);}
    bool prepare(const arc::PerceptualCapability& c)override {++trial_index;return c.target==1&&c.generation==1&&mip==0;}
    bool apply(const arc::PerceptualCapability& c)override {if(c.target!=1||c.generation!=1||c.action<1||c.action>2)return false;set_mip(c.action==1?2:3);return true;}
    bool restore(const arc::PerceptualCapability& c)noexcept override {try{if(c.target!=1||c.generation!=1)return false;set_mip(0);return true;}catch(...){return false;}}
    void finish()noexcept override{}
    std::optional<arc::ProbeCapture> capture(arc::ProbePhase phase)override {
        reset();list->SetPipelineState(pso.Get());list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        for(UINT i=0;i<repeats;++i){list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2);list->Dispatch(side/8,side/8,1);list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2+1);D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=output.Get();list->ResourceBarrier(1,&barrier);}
        transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=pixels.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=output_footprint;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,repeats*2,times.Get(),0);execute();
        arc::ProbeCapture p;p.state_key=123;p.generation=1;p.width=side;p.height=side;p.readback_complete=true;p.linear_rgb=true;p.rgb.resize(side*side*3);
        void* data{};hr(pixels->Map(0,nullptr,&data));for(UINT y=0;y<side;++y){auto* row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(data)+output_footprint.Offset+UINT64(y)*output_footprint.Footprint.RowPitch);for(UINT x=0;x<side;++x)for(UINT k=0;k<3;++k)p.rgb[(y*side+x)*3+k]=row[x*4+k];}D3D12_RANGE empty{0,0};pixels->Unmap(0,&empty);
        hr(times->Map(0,nullptr,&data));const auto* ticks=static_cast<const UINT64*>(data);std::array<UINT64,repeats*2> raw{};std::copy(ticks,ticks+raw.size(),raw.begin());times->Unmap(0,&empty);
        for(UINT i=2;i<repeats;++i){if(raw[i*2+1]<raw[i*2])throw std::runtime_error("timestamp order");p.gpu_ms.push_back(double(raw[i*2+1]-raw[i*2])*1000/double(frequency));}
        const auto stem=std::to_string(trial_index)+"-"+std::to_string(int(phase));std::ofstream binary(directory/(stem+".rgb32f"),std::ios::binary);binary.write(reinterpret_cast<const char*>(p.rgb.data()),static_cast<std::streamsize>(p.rgb.size()*sizeof(float)));
        std::ofstream meta(directory/(stem+".json"));meta<<std::setprecision(17)<<"{\"width\":"<<side<<",\"height\":"<<side<<",\"state_key\":123,\"generation\":1,\"mip\":"<<mip<<",\"frequency\":"<<frequency<<",\"ticks\":[";for(std::size_t i=0;i<raw.size();++i){if(i)meta<<',';meta<<raw[i];}meta<<"],\"gpu_ms\":[";for(std::size_t i=0;i<p.gpu_ms.size();++i){if(i)meta<<',';meta<<p.gpu_ms[i];}meta<<"]}";
        return p;
    }
};
}
int main(int argc,char** argv)try{
    std::filesystem::path path=argc>1?argv[1]:"perceptual-native";NativeProbe host(path);arc::PerceptualTrialController controller;
    // Fixture-only visibility oracle: the image outside the affected region is
    // the known constant background. Actual pixels, not object names, feed E.
    const auto reference=host.capture(arc::ProbePhase::ReferenceBefore);
    if(!reference)throw std::runtime_error("reference unavailable");
    std::size_t affected=0;
    for(std::size_t i=0;i<reference->rgb.size();i+=3)
        if(std::abs(reference->rgb[i]-.3f)>.01f||std::abs(reference->rgb[i+1]-.3f)>.01f||std::abs(reference->rgb[i+2]-.3f)>.01f)++affected;
    arc::TemporalVisibilityModel visibility;arc::VisibilityObservation observation;
    observation.id=1;observation.frame=1;observation.visible_coverage=double(affected)/(reference->width*reference->height);
    observation.local_coverage_upper=observation.visible_coverage;observation.present_reachable=true;observation.confidence=1;
    if(!visibility.observe(observation))throw std::runtime_error("visibility observation");
    arc::PerceptualCandidate c;c.capability={1,1,1,arc::PerceptualMechanism::SrvMipRange,true,true,true,true};
    c.importance=arc::VisualImportanceModel{}.evaluate(*visibility.find(1));
    auto baseline_samples=reference->gpu_ms;std::sort(baseline_samples.begin(),baseline_samples.end());
    c.measured_cost_ms=baseline_samples[baseline_samples.size()/2];c.expected_gain_ms=c.measured_cost_ms*.25;
    const auto good=controller.trial(host,c);if(!controller.restore(host))throw std::runtime_error("positive-trial restore failed");
    c.capability.action=2;const auto bad=controller.trial(host,c);
    if(bad.status!=arc::TrialStatus::Rejected||bad.verdict.reason!=arc::CriticReason::ImageDamage||controller.active()||controller.faulted())throw std::runtime_error("damaging action not safely rejected");
    std::ofstream out(path/"summary.json");out<<std::setprecision(17)<<"{\"hardware\":true,\"physical_srv_mip_change\":true,\"positive_status\":"<<int(good.status)<<",\"positive_reason\":"<<int(good.verdict.reason)<<",\"gain_ms\":"<<good.verdict.gain_ms<<",\"image_mean\":"<<good.verdict.modified_mean<<",\"image_peak\":"<<good.verdict.modified_peak<<",\"damaging_action_rejected\":true,\"restored\":true}";
    std::cout<<"Native Mega F: positive status="<<int(good.status)<<" reason="<<int(good.verdict.reason)<<" gain_ms="<<good.verdict.gain_ms<<"; damaging trial rejected and original SRV restored\n";
    return good.status==arc::TrialStatus::Retained?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
