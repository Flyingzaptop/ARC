#include "generic_readback.hpp"
#include <fstream>
#include <iomanip>
#include <limits>
#include <vector>

namespace arc::dx12::generic {
bool ImageReadback::enqueue(IDXGISwapChain* swap,ID3D12CommandQueue* queue,const std::filesystem::path& path){
    const auto start=std::chrono::steady_clock::now();path_=path;queue_=queue;
    if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    Ptr<IDXGISwapChain3> current;Ptr<ID3D12Resource> buffer;
    if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&current))))return false;
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    if(FAILED(current->GetDesc1(&swap_desc))||(swap_desc.Flags&(DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED|DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT|DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY)))return false;
    buffer_index_=current->GetCurrentBackBufferIndex();
    if(FAILED(current->GetBuffer(buffer_index_,IID_PPV_ARGS(&buffer)))||FAILED(queue->GetDevice(IID_PPV_ARGS(&device_))))return false;
    const auto desc=buffer->GetDesc();format_=desc.Format;
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1||desc.DepthOrArraySize!=1||
       !desc.Width||!desc.Height||desc.Width>7680||desc.Height>4320)return false;
    if(format_!=DXGI_FORMAT_R8G8B8A8_UNORM&&format_!=DXGI_FORMAT_B8G8R8A8_UNORM&&format_!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
    width_=static_cast<UINT>(desc.Width);height_=desc.Height;
    UINT rows{};UINT64 row_bytes{},total{};
    device_->GetCopyableFootprints(&desc,0,1,0,&footprint_,&rows,&row_bytes,&total);
    if(rows!=height_||row_bytes!=UINT64(width_)*4||total>256u*1024u*1024u)return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC data{};data.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;data.Width=total;data.Height=1;
    data.DepthOrArraySize=1;data.MipLevels=1;data.SampleDesc.Count=1;data.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&data,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_))))return false;
    data.Width=16;
    if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&data,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&timestamps_))))return false;
    D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=2;
    if(FAILED(device_->CreateQueryHeap(&query,IID_PPV_ARGS(&queries_)))||FAILED(queue->GetTimestampFrequency(&frequency_))||!frequency_)return false;
    if(FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)))||
       FAILED(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list_)))||
       FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return false;
    list_->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list_->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=buffer.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback_.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint_;
    list_->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
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
    void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(footprint_.Footprint.RowPitch)*(height_-1)+SIZE_T(width_)*4};
    if(FAILED(readback_->Map(0,&range,&mapped)))return false;
    const auto pixels_path=std::filesystem::path(path_.wstring()+L".pixels");
    std::ofstream pixels(pixels_path,std::ios::binary);
    for(UINT y=0;y<height_;++y)pixels.write(static_cast<const char*>(mapped)+footprint_.Offset+SIZE_T(y)*footprint_.Footprint.RowPitch,SIZE_T(width_)*4);
    D3D12_RANGE empty{0,0};readback_->Unmap(0,&empty);pixels.close();if(!pixels)return false;
    range={0,16};if(FAILED(timestamps_->Map(0,&range,&mapped)))return false;
    const auto* values=static_cast<const UINT64*>(mapped);const bool valid=values[1]>=values[0];
    const auto elapsed=values[1]-values[0];timestamps_->Unmap(0,&empty);if(!valid)return false;
    const auto temporary=std::filesystem::path(path_.wstring()+L".tmp");
    std::ofstream out(temporary);out<<std::setprecision(12)<<"{\"schema\":1,\"readback_complete\":true,\"width\":"<<width_
        <<",\"height\":"<<height_<<",\"dxgi_format\":"<<format_<<",\"bytes_per_pixel\":4,\"row_bytes\":"<<width_*4
        <<",\"buffer_index\":"<<buffer_index_<<",\"pixel_file\":"<<std::quoted(pixels_path.filename().string())
        <<",\"copy_gpu_ms\":"<<double(elapsed)*1000.0/double(frequency_)<<",\"enqueue_cpu_ms\":"<<enqueue_cpu_ms_
        <<",\"present_hresult\":"<<present_result_<<",\"gpu_frame_timing_available\":false,\"color_space_known\":false,\"reproducible_state\":false,\"quality_mutations\":0}";
    out.close();if(!out)return false;std::filesystem::rename(temporary,path_);return true;
}
}
