#include "arc/perceptual_trial.hpp"
#include "arc/predictive_perceptual.hpp"
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
#include <chrono>
using Microsoft::WRL::ComPtr;
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("D3D12 HRESULT "+std::to_string(r));}
class FrameExperiment final:public arc::PerceptualProbeHost {
    static constexpr UINT width=1920, height=1080, texture_side=4096, mip_count=4;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> texture,output,lighting,shadows,pixels,times;
    ComPtr<IDXGISwapChain3> swap;
    std::array<ComPtr<ID3D12Resource>,2> backbuffers;
    HWND window{};
    UINT material_samples=16,light_steps=16,shadow_rays=4,scenario=0,action_mask=0;
    bool isolated=true;
    struct Frame {double wall{};std::array<UINT64,6> ticks{};};
    std::vector<Frame> last_frames;
    static double median(std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];}
    double gpu(const Frame& f)const{return double(f.ticks[5]-f.ticks[0])*1000/double(frequency);}
    void barrier(ID3D12Resource* r){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;list->ResourceBarrier(1,&b);}
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
    UINT64 fence_value{},frequency{};
    UINT descriptor_stride{};
    HANDLE event{};
    unsigned mip{},trial_index{};
    unsigned visible_side{16};
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
    explicit FrameExperiment(std::filesystem::path path,UINT scene):scenario(scene),directory(std::move(path)){
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
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=4;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));descriptor_stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=texture_side;td.Height=texture_side;td.DepthOrArraySize=1;td.MipLevels=mip_count;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,mip_count> footprints{};UINT64 upload_bytes{};
        device->GetCopyableFootprints(&td,0,mip_count,0,footprints.data(),nullptr,nullptr,&upload_bytes);
        auto upload=buffer(upload_bytes,D3D12_HEAP_TYPE_UPLOAD);void* data{};D3D12_RANGE empty{0,0};hr(upload->Map(0,&empty,&data));
        // Real downsampled mips: high-detail control averages its checkerboard;
        // smooth material retains its low-frequency gradient.
        for(UINT level=0;level<mip_count;++level){const auto& f=footprints[level];
            for(UINT y=0;y<f.Footprint.Height;++y){auto* row=static_cast<unsigned char*>(data)+f.Offset+UINT64(y)*f.Footprint.RowPitch;
                for(UINT x=0;x<f.Footprint.Width;++x){
                    if(scenario==2){const unsigned char value=level<2?((((x<<level)/2+(y<<level)/2)%2)?230:26):128;row[x*4]=row[x*4+1]=row[x*4+2]=value;}
                    else {row[x*4]=static_cast<unsigned char>(80+64*x/f.Footprint.Width);row[x*4+1]=static_cast<unsigned char>(80+64*y/f.Footprint.Height);row[x*4+2]=112;}
                    row[x*4+3]=255;
                }
            }
        }
        upload->Unmap(0,nullptr);
        for(UINT level=0;level<mip_count;++level){D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[level];D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=level;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
        transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        td.Width=width;td.Height=height;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
        UINT64 bytes{};device->GetCopyableFootprints(&td,0,1,0,&output_footprint,nullptr,nullptr,&bytes);pixels=buffer(bytes,D3D12_HEAP_TYPE_READBACK);times=buffer(6*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=td.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;auto handle=heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=descriptor_stride;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,handle);
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&lighting)));
        uav.Format=td.Format;handle.ptr+=descriptor_stride;device->CreateUnorderedAccessView(lighting.Get(),nullptr,&uav,handle);
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&shadows)));
        handle.ptr+=descriptor_stride;device->CreateUnorderedAccessView(shadows.Get(),nullptr,&uav,handle);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,1};
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={2,ranges};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_PARAMETER parameters[2]{parameter,{}};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants.Num32BitValues=6;parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> blob,errors;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        const char* shader=R"(
Texture2D<float4> source:register(t0); RWTexture2D<float4> output_image:register(u0);
RWTexture2D<float4> light_image:register(u1); RWTexture2D<float4> shadow_image:register(u2); SamplerState linear_sampler:register(s0);
cbuffer FrameInput:register(b0) {uint stage;uint material_samples;uint light_steps;uint detailed;uint shadow_rays;uint shadow_scale;};
[numthreads(8,8,1)] void main(uint3 tid:SV_DispatchThreadID) {
 if(tid.x>=1920||tid.y>=1080)return;
 float2 uv=(float2(tid.xy)+.5)/float2(1920,1080);
 if(stage==0){
  float3 l=0;
  [loop]for(uint j=0;j<light_steps;++j){
   float2 position=frac(float2(j*.6180339,j*.4142135));
   float2 delta=uv-position;
   float energy=rcp(.03+dot(delta,delta));
   l+=energy*(.35+.65*float3(position,.5));
  }
  light_image[tid.xy]=float4(.15+.8*(l/(light_steps*10+l)),1);return;
 }
 if(stage==1){
  float3 sum=0;
  [loop]for(uint i=0;i<material_samples;++i){
   float2 address=detailed!=0?uv:(uv*.7+.05+.2*frac(float2(i*.6180339,i*.4142135)));
   // Every material sample contributes; no masked/discarded full-screen work.
   sum+=source.SampleLevel(linear_sampler,address,0).rgb;
  }
  output_image[tid.xy]=float4((sum/material_samples)*light_image[tid.xy].rgb,1);return;
 }
 if(stage==2){
  uint2 extent=uint2((1920+shadow_scale-1)/shadow_scale,(1080+shadow_scale-1)/shadow_scale);
  if(any(tid.xy>=extent))return;
  float2 screen=(float2(tid.xy)+.5)/float2(extent);
  float3 origin=float3((screen.x-.5)*12,0,(screen.y-.5)*8);
  float visibility=0;
  [loop]for(uint ray=0;ray<shadow_rays;++ray){
   float2 sample_pos=frac(float2(ray*.6180339+.17,ray*.4142135+.31));
   float3 target=float3(-2+sample_pos.x*4,6,1+sample_pos.y*3);
   float3 direction=normalize(target-origin);float maximum=length(target-origin);bool blocked=false;
   [unroll]for(uint object=0;object<8;++object){
    float3 center=float3(-4+(object%4)*2.5,.6+(object%3)*.2,-2+(object/4)*3.5);
    float radius=.45+.12*(object%3);float3 offset=origin-center;
    float b=dot(offset,direction);float discriminant=b*b-dot(offset,offset)+radius*radius;
    if(discriminant>0){float t=-b-sqrt(discriminant);blocked=blocked||(t>0&&t<maximum);}
   }
   visibility+=blocked?0:1;
  }
  shadow_image[tid.xy]=float4(visibility/shadow_rays,0,0,1);return;
 }
 float visibility=shadow_image[tid.xy/shadow_scale].x;
 float3 color=output_image[tid.xy].rgb*(.3+.7*visibility);
 float vignette=1-.2*dot(uv-.5,uv-.5);
 output_image[tid.xy]=float4(saturate(color*vignette),1);
})";
        hr(D3DCompile(shader,std::strlen(shader),"portable-probe",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
        D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=6;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));
        execute();set_mip(0);
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ARCFullFrameTest";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"ARC 1080p full-frame test",WS_OVERLAPPEDWINDOW,0,0,width,height,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)throw std::runtime_error("window");
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=width;sd.Height=height;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> base;hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&base));hr(base.As(&swap));
        for(UINT i=0;i<2;++i)hr(swap->GetBuffer(i,IID_PPV_ARGS(&backbuffers[i])));

    }
    ~FrameExperiment(){if(event)CloseHandle(event);swap.Reset();if(window)DestroyWindow(window);}
    Frame render(){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        const auto start=std::chrono::steady_clock::now();
        reset();list->SetPipelineState(pso.Get());list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        for(UINT stage=0;stage<4;++stage){
            list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,stage);
            const UINT scale=1u<<((action_mask>>12)&3u);
            const UINT params[]{stage,material_samples,std::max(1u,light_steps>>( (action_mask>>4)&3u)),scenario==2?1u:0u,std::max(1u,shadow_rays>>((action_mask>>8)&3u)),scale};
            list->SetComputeRoot32BitConstants(1,6,params,0);
            const UINT divisor=stage==2?scale:1;
            list->Dispatch(((width+divisor-1)/divisor+7)/8,((height+divisor-1)/divisor+7)/8,1);
            barrier(stage==0?lighting.Get():stage==2?shadows.Get():output.Get());
        }
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,4);
        // Isolated critic probes do not alter the displayed swapchain.
        if(!isolated){
            auto* back=backbuffers[swap->GetCurrentBackBufferIndex()].Get();
            transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(back,output.Get());
            transition(back,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
            transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,5);
        list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,6,times.Get(),0);execute();
        if(!isolated)hr(swap->Present(0,0));
        Frame f;f.wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        void* data{};hr(times->Map(0,nullptr,&data));std::copy_n(static_cast<UINT64*>(data),6,f.ticks.begin());D3D12_RANGE empty{0,0};times->Unmap(0,&empty);
        for(UINT i=1;i<6;++i)if(f.ticks[i]<f.ticks[i-1])throw std::runtime_error("timestamp order");return f;
    }
    arc::ProbeCapture read_image(){
        reset();transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=pixels.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=output_footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);execute();
        arc::ProbeCapture p;p.width=width;p.height=height;p.generation=1;p.state_key=1+scenario;p.readback_complete=true;p.linear_rgb=true;p.rgb.resize(std::size_t(width)*height*3);
        void* data{};hr(pixels->Map(0,nullptr,&data));
        for(UINT y=0;y<height;++y){const auto* row=static_cast<unsigned char*>(data)+output_footprint.Offset+UINT64(y)*output_footprint.Footprint.RowPitch;
            for(UINT x=0;x<width;++x)for(UINT k=0;k<3;++k)p.rgb[(std::size_t(y)*width+x)*3+k]=float(row[x*4+k])/255;}
        D3D12_RANGE empty{0,0};pixels->Unmap(0,&empty);return p;
    }
    void save(const std::string& name,const arc::ProbeCapture& p){
        std::ofstream file(directory/(name+".rgb8"),std::ios::binary);
        for(float x:p.rgb){const unsigned char byte=static_cast<unsigned char>(std::lround(x*255));file.write(reinterpret_cast<const char*>(&byte),1);}
    }
    void timings(const std::string& name,const std::vector<Frame>& frames){
        std::ofstream file(directory/(name+".json"));file<<std::setprecision(17)<<"{\"width\":"<<width<<",\"height\":"<<height<<",\"frequency\":"<<frequency<<",\"mip\":"<<mip<<",\"isolated\":"<<(isolated?"true":"false")<<",\"material_samples\":"<<material_samples<<",\"light_steps\":"<<light_steps<<",\"shadow_rays\":"<<shadow_rays<<",\"action_mask\":"<<action_mask<<",\"frames\":[";
        for(std::size_t i=0;i<frames.size();++i){if(i)file<<',';const auto& f=frames[i];file<<"{\"wall_ms\":"<<f.wall<<",\"ticks\":[";for(UINT k=0;k<6;++k){if(k)file<<',';file<<f.ticks[k];}file<<"]}";}file<<"]}";
    }
    void warmup(){
        // Equal elapsed warmup for all arms avoids short cheap-arm warmup and
        // clock-state changes after image readback/file IO contaminating references.
        const auto start=std::chrono::steady_clock::now();
        do{render();}while(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<1.0);
    }
    void calibrate(){
        isolated=false;set_mip(0);std::ofstream log(directory/"calibration.csv");log<<"material_samples,light_steps,shadow_rays,wall_ms,lighting_ms,material_ms\n";
        // Only baseline timings guide calibration; no degraded trial has run yet.
        material_samples=(scenario==1||scenario==3)?2:16;light_steps=scenario==1?256:16;shadow_rays=scenario==3?128:4;
        for(UINT pass=0;pass<5;++pass){
            warmup();std::vector<Frame> frames;std::vector<double> wall;
            for(int i=0;i<7;++i){frames.push_back(render());wall.push_back(frames.back().wall);}
            const double ms=median(wall);const auto& f=frames[3];
            log<<material_samples<<','<<light_steps<<','<<shadow_rays<<','<<ms<<','<<double(f.ticks[1]-f.ticks[0])*1000/frequency<<','<<double(f.ticks[2]-f.ticks[1])*1000/frequency<<'\n';log.flush();
            if(ms>=30&&ms<=36)break;
            auto& knob=scenario==1?light_steps:scenario==3?shadow_rays:material_samples;
            const auto next=std::clamp<UINT>(static_cast<UINT>(std::lround(knob*33.3/std::max(.1,ms))),1,scenario==1?8192u:2048u);
            if(next==knob)break;knob=next;
        }
        isolated=true;
    }
    bool prepare(const arc::PerceptualCapability& c)override {isolated=true;++trial_index;return c.generation==1&&c.target==1&&mip==0;}
    bool apply(const arc::PerceptualCapability& c)override {if(c.generation!=1||c.target!=1||!c.action||c.action>0x3333)return false;action_mask=static_cast<UINT>(c.action);set_mip(action_mask&3u);return true;}
    bool restore(const arc::PerceptualCapability& c)noexcept override {try{if(c.generation!=1||c.target!=1)return false;action_mask=0;set_mip(0);return true;}catch(...){return false;}}
    void finish()noexcept override{}
    std::optional<arc::ProbeCapture> capture(arc::ProbePhase phase)override {
        warmup();last_frames.clear();for(int i=0;i<21;++i)last_frames.push_back(render());
        auto p=read_image();for(const auto& f:last_frames)p.gpu_ms.push_back(gpu(f));
        const auto name="probe-"+std::to_string(trial_index)+"-"+std::to_string(int(phase));save(name,p);timings(name,last_frames);return p;
    }
    void run(){
        calibrate();arc::PerceptualCandidate c;c.capability={2,1,1,arc::PerceptualMechanism::SrvMipRange,true,true,true,true,true};
        c.importance.id=1;c.importance.score=1;c.importance.confidence=1;
        c.measured_cost_ms=gpu(render());c.expected_gain_ms=c.measured_cost_ms*.5;
        arc::PerceptualTrialController controller;
        const auto begin=std::chrono::steady_clock::now();
        arc::PerceptualTrialResult trial;UINT selected_mask=0,selected_probe=1;
        std::array<UINT,4> domain_best{};std::array<double,4> domain_gain{};
        std::vector<UINT> actions{2,3,0x10,0x20,0x100,0x200,0x1000,0x2000};
        std::ofstream candidates(directory/"candidates.json");candidates<<"[";
        for(std::size_t index=0;index<actions.size();++index){
            const UINT action=actions[index];c.capability.action=action;
            c.capability.mechanism=(action<=3)?arc::PerceptualMechanism::SrvMipRange:arc::PerceptualMechanism::HostDefined;
            const auto result=controller.trial(*this,c);
            if(!controller.restore(*this))throw std::runtime_error("rollback failure");
            if(index==0 || (result.status==arc::TrialStatus::Retained &&
                (trial.status!=arc::TrialStatus::Retained||result.verdict.gain_ms>trial.verdict.gain_ms))){trial=result;selected_mask=action;selected_probe=trial_index;}
            if(index<8 && result.status==arc::TrialStatus::Retained){
                const auto domain=index/2;if(result.verdict.gain_ms>domain_gain[domain]){domain_best[domain]=action;domain_gain[domain]=result.verdict.gain_ms;}
            }
            if(index)candidates<<',';
            candidates<<std::setprecision(17)<<"{\"probe\":"<<trial_index<<",\"action_mask\":"<<action<<",\"status\":"<<int(result.status)<<",\"reason\":"<<int(result.verdict.reason)<<",\"gain_ms\":"<<result.verdict.gain_ms<<'}';
            candidates.flush();
            if(index==7){UINT combined=0,count=0;for(auto best:domain_best)if(best){combined|=best;++count;}if(count>1)actions.push_back(combined);}
        }
        candidates<<"]";candidates.close();
        const double trial_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        // Counterbalanced capacity measurement, even if the critic rejected it:
        // rejected modified images are never presented to a game/user workload.
        for(UINT round=0;round<3;++round)for(UINT arm=0;arm<4;++arm){
            const bool modified=round%2?(arm==0||arm==3):(arm==1||arm==2);
            action_mask=modified?selected_mask:0;set_mip(action_mask&3u);isolated=false;warmup();
            std::vector<Frame> frames;for(int i=0;i<21;++i)frames.push_back(render());
            timings("round-"+std::to_string(round)+"-arm-"+std::to_string(arm),frames);
            if(round==0&&(arm==0||arm==1||arm==3))save("frame-arm-"+std::to_string(arm),read_image());
        }
        action_mask=0;set_mip(0);isolated=false;render();
        std::ofstream summary(directory/"trial.json");summary<<std::setprecision(17)<<"{\"scenario\":"<<scenario<<",\"selected_probe\":"<<selected_probe<<",\"selected_mip\":"<<(selected_mask&3u)<<",\"selected_action\":"<<selected_mask<<",\"status\":"<<int(trial.status)<<",\"reason\":"<<int(trial.verdict.reason)<<",\"probe_wall_ms\":"<<trial_ms<<",\"image_mean\":"<<trial.verdict.modified_mean<<",\"image_peak\":"<<trial.verdict.modified_peak<<",\"image_tile\":"<<trial.verdict.modified_tile<<",\"restored\":true,\"resolution\":[1920,1080],\"present_sync\":0,\"game_fps_claim\":false}";
        std::cout<<"scenario="<<scenario<<" samples="<<material_samples<<" light="<<light_steps<<" decision="<<int(trial.status)<<" reason="<<int(trial.verdict.reason)<<std::endl;
    }
};
}
int main(int argc,char** argv)try{
    if(argc!=3)throw std::runtime_error("Usage: arc-full-frame-x2 <new output directory> <scenario 0..3>");
    const int scenario=std::stoi(argv[2]);if(scenario<0||scenario>3)throw std::runtime_error("scenario");
    if(std::filesystem::exists(argv[1]))throw std::runtime_error("Use a new evidence directory");
    FrameExperiment experiment(argv[1],static_cast<UINT>(scenario));experiment.run();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
