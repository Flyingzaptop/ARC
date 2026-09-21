#include "generic_readback.hpp"
#include "generic_optimizer.hpp"
#include <fstream>
#include <iomanip>
#include <limits>
#include <vector>
#include <d3dcompiler.h>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <map>

namespace arc::dx12::generic {
namespace {
Microsoft::WRL::ComPtr<ID3DBlob> feature_shader;
std::once_flag feature_shader_once;
struct FeaturePipeline {Microsoft::WRL::ComPtr<ID3D12Device> device;Microsoft::WRL::ComPtr<ID3D12RootSignature> root;Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;};
struct FeatureCache {std::mutex mutex;std::map<ID3D12Device*,FeaturePipeline> devices;};
FeatureCache& feature_cache(){static auto* cache=new FeatureCache;return *cache;}

constexpr char feature_source[]=R"(
Texture2D<float4> Source:register(t0);
RWStructuredBuffer<float4> Tiles:register(u0);
cbuffer Extent:register(b0){uint Width;uint Height;uint TilesX;}
groupshared float4 Shared[64];
float luma(uint2 p){return dot(Source.Load(int3(p,0)).rgb,float3(.2126,.7152,.0722));}
[numthreads(8,8,1)]void main(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID,uint index:SV_GroupIndex){
 float4 s=0;
 [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
  uint2 p=group.xy*16+thread.xy*2+uint2(x,y);
  if(p.x<Width&&p.y<Height){float v=luma(p);
   float edge=max(abs(v-luma(uint2(min(p.x+1,Width-1),p.y))),abs(v-luma(uint2(p.x,min(p.y+1,Height-1)))));
   s+=float4(v,v*v,0,1);s.z=max(s.z,edge);
  }
 }
 Shared[index]=s;GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint stride=32;stride>0;stride>>=1){if(index<stride){float4 b=Shared[index+stride];Shared[index].xy+=b.xy;Shared[index].z=max(Shared[index].z,b.z);Shared[index].w+=b.w;}GroupMemoryBarrierWithGroupSync();}
 if(index==0){s=Shared[0];float mean=s.x/max(s.w,1);Tiles[group.y*TilesX+group.x]=float4(mean,max(0,s.y/max(s.w,1)-mean*mean),s.z,s.w);}
})";
}
bool ImageReadback::prepare_features(){
    std::call_once(feature_shader_once,[]{Microsoft::WRL::ComPtr<ID3DBlob> errors;
        D3DCompile(feature_source,sizeof(feature_source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&feature_shader,&errors);});
    return feature_shader!=nullptr;
}
bool ImageReadback::prepare_feature_device(ID3D12Device* device){
    if(!device||!prepare_features())return false;
    auto& cache=feature_cache();{std::lock_guard lock(cache.mutex);if(cache.devices.contains(device))return true;if(cache.devices.size()>=4)return false;}
    FeaturePipeline prepared;prepared.device=device;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,3};
    for(UINT i=0;i<2;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,&ranges[i]};}
    D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=3;root.pParameters=parameters;Ptr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&prepared.root))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=prepared.root.Get();pipeline.CS={feature_shader->GetBufferPointer(),feature_shader->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&prepared.pipeline))))return false;
    // Driver compilation must not hold a mutex needed by Present.
    std::lock_guard lock(cache.mutex);if(cache.devices.contains(device))return true;if(cache.devices.size()>=4)return false;cache.devices.emplace(device,std::move(prepared));return true;
}
bool ImageReadback::enqueue(IDXGISwapChain* swap,ID3D12CommandQueue* queue,const std::filesystem::path& path,std::uint64_t mutations,UINT rate,bool features){
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);capture_qpc_=static_cast<UINT64>(qpc.QuadPart);
    const bool completed=ready(),old_features=features_;const auto old_width=width_,old_height=height_,old_format=format_;const auto* old_device=device_.Get();
    submitted_=present_seen_=false;signal_result_=present_result_=E_PENDING;
    const auto start=std::chrono::steady_clock::now();path_=path;queue_=queue;mutations_=mutations;experimental_rate_=rate;features_=features;
    if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    Ptr<IDXGISwapChain3> current;Ptr<ID3D12Resource> buffer;
    if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&current))))return false;
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    if(FAILED(current->GetDesc1(&swap_desc))||(swap_desc.Flags&(DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED|DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT|DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY)))return false;
    buffer_index_=current->GetCurrentBackBufferIndex();
    if(FAILED(current->GetBuffer(buffer_index_,IID_PPV_ARGS(&buffer)))||FAILED(queue->GetDevice(IID_PPV_ARGS(&device_))))return false;
    capture_queue_=reinterpret_cast<UINT64>(queue);capture_backbuffer_=reinterpret_cast<UINT64>(buffer.Get());
    if(features)execution_.clear();
    const auto desc=buffer->GetDesc();format_=desc.Format;
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1||desc.DepthOrArraySize!=1||
       !desc.Width||!desc.Height||desc.Width>7680||desc.Height>4320)return false;
    if(format_!=DXGI_FORMAT_R8G8B8A8_UNORM&&format_!=DXGI_FORMAT_B8G8R8A8_UNORM&&format_!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
    width_=static_cast<UINT>(desc.Width);height_=desc.Height;
    reused_storage_=completed&&old_features==features_&&old_width==width_&&old_height==height_&&old_format==format_&&old_device==device_.Get()&&SUCCEEDED(device_->GetDeviceRemovedReason());
    if(!features_){feature_heap_.Reset();feature_output_.Reset();feature_root_.Reset();feature_pipeline_.Reset();}
    if(features_&&((desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!feature_shader))return false;
    UINT rows{};UINT64 row_bytes{},total{};
    device_->GetCopyableFootprints(&desc,0,1,0,&footprint_,&rows,&row_bytes,&total);
    if(rows!=height_||row_bytes!=UINT64(width_)*4||total>256u*1024u*1024u)return false;
    tiles_x_=(width_+15)/16;tiles_y_=(height_+15)/16;if(features_)total=UINT64(tiles_x_)*tiles_y_*16;output_bytes_=total;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC data{};data.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;data.Width=total;data.Height=1;
    data.DepthOrArraySize=1;data.MipLevels=1;data.SampleDesc.Count=1;data.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(!reused_storage_&&FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&data,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_))))return false;
    if(features_){
        D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;data.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(!reused_storage_&&FAILED(device_->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&data,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&feature_output_))))return false;
        data.Flags=D3D12_RESOURCE_FLAG_NONE;
        {auto& cache=feature_cache();std::lock_guard lock(cache.mutex);auto found=cache.devices.find(device_.Get());if(found==cache.devices.end())return false;feature_root_=found->second.root;feature_pipeline_=found->second.pipeline;}
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(!reused_storage_&&FAILED(device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&feature_heap_))))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=desc.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        auto handle=feature_heap_->GetCPUDescriptorHandleForHeapStart();device_->CreateShaderResourceView(buffer.Get(),&srv,handle);handle.ptr+=device_->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=tiles_x_*tiles_y_;uav.Buffer.StructureByteStride=16;
        device_->CreateUnorderedAccessView(feature_output_.Get(),nullptr,&uav,handle);
    }
    data.Width=16;
    if(!reused_storage_&&FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&data,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&timestamps_))))return false;
    D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=2;
    if((!reused_storage_&&FAILED(device_->CreateQueryHeap(&query,IID_PPV_ARGS(&queries_))))||FAILED(queue->GetTimestampFrequency(&frequency_))||!frequency_)return false;
    if(FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)))||
       FAILED(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list_)))||
       FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return false;
    list_->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,features_?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE};
    list_->ResourceBarrier(1,&barrier);
    if(features_){
        if(reused_storage_){D3D12_RESOURCE_BARRIER reuse{};reuse.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;reuse.Transition={feature_output_.Get(),0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};list_->ResourceBarrier(1,&reuse);}
        list_->SetPipelineState(feature_pipeline_.Get());list_->SetComputeRootSignature(feature_root_.Get());ID3D12DescriptorHeap* heaps[]{feature_heap_.Get()};list_->SetDescriptorHeaps(1,heaps);
        const UINT extent[]{width_,height_,tiles_x_};list_->SetComputeRoot32BitConstants(0,3,extent,0);
        auto handle=feature_heap_->GetGPUDescriptorHandleForHeapStart();list_->SetComputeRootDescriptorTable(1,handle);handle.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);list_->SetComputeRootDescriptorTable(2,handle);
        list_->Dispatch(tiles_x_,tiles_y_,1);
        D3D12_RESOURCE_BARRIER ready{};ready.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;ready.Transition={feature_output_.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list_->ResourceBarrier(1,&ready);
        list_->CopyResource(readback_.Get(),feature_output_.Get());
    }else{
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=buffer.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback_.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint_;
        list_->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list_->ResourceBarrier(1,&barrier);
    list_->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
    list_->ResolveQueryData(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamps_.Get(),0);
    if(FAILED(list_->Close()))return false;
    ID3D12CommandList* commands[]{list_.Get()};queue->ExecuteCommandLists(1,commands);submitted_=true;
    signal_result_=queue->Signal(fence_.Get(),1);
    enqueue_cpu_ms_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    // The swapchain owns the source. Our copy precedes this Present on its own
    // direct queue, so normal application resize/destruction synchronization also
    // covers the copy. Do not retain a backbuffer COM reference across Present:
    // that would make an otherwise legal ResizeBuffers fail.
    return SUCCEEDED(signal_result_);
}
bool ImageReadback::ready()const noexcept{
    if(!submitted_||!present_seen_)return false;
    // Never destroy allocator/readback resources on a fence-signal failure while
    // the GPU may still reference them. One bounded job stays quarantined until
    // device removal; subsequent captures are refused.
    if(FAILED(device_->GetDeviceRemovedReason()))return true;
    return SUCCEEDED(signal_result_)&&fence_->GetCompletedValue()>=1;
}
bool ImageReadback::write(){
    if(!ready()||FAILED(present_result_)||FAILED(device_->GetDeviceRemovedReason())||FAILED(signal_result_))return false;
    void* mapped{};D3D12_RANGE range{0,features_?static_cast<SIZE_T>(output_bytes_):static_cast<SIZE_T>(footprint_.Footprint.RowPitch)*(height_-1)+SIZE_T(width_)*4};
    if(FAILED(readback_->Map(0,&range,&mapped)))return false;
    const auto pixels_path=std::filesystem::path(path_.wstring()+(features_?L".tiles":L".pixels"));
    std::ofstream pixels(pixels_path,std::ios::binary);
    double sum=0,count=0,max_edge=0;
    if(features_){
        pixels.write(static_cast<const char*>(mapped),output_bytes_);const auto* tile=static_cast<const float*>(mapped);
        for(UINT i=0;i<tiles_x_*tiles_y_;++i){if(!std::isfinite(tile[4*i])||!std::isfinite(tile[4*i+1])||!std::isfinite(tile[4*i+2])||!std::isfinite(tile[4*i+3])||tile[4*i+3]<1||tile[4*i+3]>256){D3D12_RANGE empty{0,0};readback_->Unmap(0,&empty);return false;}sum+=double(tile[4*i])*tile[4*i+3];count+=tile[4*i+3];max_edge=std::max(max_edge,double(tile[4*i+2]));}
    }else for(UINT y=0;y<height_;++y)pixels.write(static_cast<const char*>(mapped)+footprint_.Offset+SIZE_T(y)*footprint_.Footprint.RowPitch,SIZE_T(width_)*4);
    D3D12_RANGE empty{0,0};readback_->Unmap(0,&empty);pixels.close();if(!pixels)return false;
    if(features_&&count!=double(width_)*height_)return false;
    range={0,16};if(FAILED(timestamps_->Map(0,&range,&mapped)))return false;
    const auto* values=static_cast<const UINT64*>(mapped);const bool valid=values[1]>=values[0];
    const auto elapsed=values[1]-values[0];timestamps_->Unmap(0,&empty);if(!valid)return false;
    const auto temporary=std::filesystem::path(path_.wstring()+L".tmp");
    std::ofstream out(temporary);out<<std::setprecision(12)<<"{\"schema\":1,\"readback_complete\":true,\"gpu_features_only\":"<<(features_?"true":"false")<<",\"width\":"<<width_
        <<",\"height\":"<<height_<<",\"dxgi_format\":"<<format_<<",\"buffer_index\":"<<buffer_index_;
    if(features_)out<<",\"tile_file\":"<<std::quoted(pixels_path.filename().string());
    else out<<",\"bytes_per_pixel\":4,\"row_bytes\":"<<width_*4<<",\"pixel_file\":"<<std::quoted(pixels_path.filename().string())<<",\"copy_gpu_ms\":"<<double(elapsed)*1000.0/double(frequency_);
    out<<",\"analysis_gpu_ms\":"<<double(elapsed)*1000.0/double(frequency_)<<",\"enqueue_cpu_ms\":"<<enqueue_cpu_ms_<<",\"reused_storage\":"<<(reused_storage_?"true":"false");
    if(features_)out<<",\"full_image_readback\":false,\"tile_size\":16,\"tiles_x\":"<<tiles_x_<<",\"tiles_y\":"<<tiles_y_<<",\"readback_bytes\":"<<output_bytes_<<",\"tile_layout\":\"float32_le_mean_variance_max_edge_pixel_count\",\"pixel_count\":"<<count<<",\"mean_encoded_luma\":"<<(count?sum/count:0)<<",\"max_encoded_edge\":"<<max_edge;
    out<<",\"gpu_execution\":{\"kind\":\"shader_written_epoch\",\"capture_queue\":"<<capture_queue_<<",\"backbuffer_identity\":"<<capture_backbuffer_<<",\"image_fence_completed\":true,\"markers\":[";
    bool first_marker=true;
    for(const auto& proof:execution_){std::array<std::array<UINT64,2>,optimizer::GpuControl::capacity> actual{};std::array<std::array<UINT,2>,optimizer::GpuControl::capacity> spatial{};const bool complete=proof.read(actual,&spatial);
        for(unsigned i=0;i<proof.expected.size();++i){const auto& expected=proof.expected[i];if(!expected[0])continue;if(!first_marker)out<<',';first_marker=false;
            out<<"{\"expected_epoch\":"<<expected[0]<<",\"expected_pipeline\":"<<expected[1]<<",\"actual_epoch\":"<<actual[i][0]<<",\"actual_pipeline\":"<<actual[i][1]<<",\"coarse_tiles\":"<<spatial[i][0]<<",\"evaluated_tiles\":"<<spatial[i][1]<<",\"queue\":"<<proof.queue<<",\"fence_completed\":"<<(complete?"true":"false")<<",\"matched\":"<<(complete&&actual[i]==expected&&proof.queue==capture_queue_?"true":"false")<<'}';}}
    out<<"]}";
    out
        <<",\"present_hresult\":"<<present_result_<<",\"gpu_frame_timing_available\":false,\"capture_qpc\":"<<capture_qpc_<<",\"color_space_known\":"<<(color_space_known_?"true":"false")<<",\"color_space\":"<<color_space_<<",\"reproducible_state\":false,\"cumulative_modified_draws_at_copy\":"<<mutations_<<",\"experimental_vrs_rate_at_copy\":"<<experimental_rate_<<'}';
    out.close();if(!out)return false;std::filesystem::rename(temporary,path_);return true;
}
}
