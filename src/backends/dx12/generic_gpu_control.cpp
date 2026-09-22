#include "generic_gpu_control.hpp"
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstddef>

namespace arc::dx12::optimizer {
namespace {
constexpr UINT64 prepass_offset=GpuControl::capacity*D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr UINT64 block_bytes=prepass_offset*4;
constexpr UINT64 model_stride=4352,model_block_bytes=model_stride*GpuControl::capacity;
constexpr auto control_state=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_PREDICATION;
constexpr UINT calibration_queries=GpuControl::capacity*4+2;
constexpr UINT64 calibration_bytes=calibration_queries*sizeof(UINT64);
constexpr UINT64 marker_bytes=GpuControl::capacity*32;
constexpr UINT64 spatial_page_bytes=GpuControl::spatial_capacity*GpuControl::spatial_record_bytes;
void check(HRESULT result){if(FAILED(result))throw std::runtime_error("GPU control HRESULT "+std::to_string(result));}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
void copy_policy(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,ID3D12Resource* source,UINT64 offset){
    barrier(list,buffer,control_state,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(buffer,0,source,offset,block_bytes);
    barrier(list,buffer,D3D12_RESOURCE_STATE_COPY_DEST,control_state);
}
}
GpuControl::GpuControl(ID3D12Device* device,bool measure,bool spatial):device_(device){
    static_assert(offsetof(ControlValue,proof_epoch)==64&&offsetof(ControlValue,spatial_capacity)==80&&sizeof(ControlValue)==144&&offsetof(ControlValue,probe_epoch)==112&&offsetof(ControlValue,model_key)==128);
    if(!device||device->GetNodeCount()!=1)throw std::runtime_error("GPU control requires one node");
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=block_bytes;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,control_state,nullptr,IID_PPV_ARGS(&buffer_)));
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&neutral_)));
    d.Width*=4;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload_)));
    void* data{};D3D12_RANGE none{};check(neutral_->Map(0,&none,&data));std::memset(data,0,block_bytes);
    for(unsigned i=0;i<capacity;++i){ControlValue neutral;std::memcpy(static_cast<char*>(data)+i*256,&neutral,sizeof(neutral));neutral.spatial_flags=16;std::memcpy(static_cast<char*>(data)+prepass_offset+i*256,&neutral,sizeof(neutral));}neutral_->Unmap(0,nullptr);
    if(spatial){models_=std::make_unique<std::array<arc::SpatialModelTable,capacity>>();d.Width=model_block_bytes*4;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&model_upload_)));}
    marker_stride_=32+(spatial?2ull*spatial_capacity*spatial_record_bytes+model_stride:0);
    hp.Type=D3D12_HEAP_TYPE_DEFAULT;d.Width=marker_stride_*capacity;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&marker_)));
    hp.Type=D3D12_HEAP_TYPE_READBACK;d.Width=marker_bytes*4;d.Flags=D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&marker_readback_)));
    if(spatial){d.Width=spatial_page_bytes*2*4;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&spatial_readback_)));}
    for(auto& lease:marker_leases_)lease=std::make_shared<unsigned>(0);
    if(measure){
        calibrations_.reserve(512);
        D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=8;
        check(device->CreateQueryHeap(&query,IID_PPV_ARGS(&timestamps_)));
        hp.Type=D3D12_HEAP_TYPE_READBACK;d.Width=8*sizeof(UINT64);
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_)));
        query.Count=calibration_queries;check(device->CreateQueryHeap(&query,IID_PPV_ARGS(&calibration_queries_)));
        d.Width=calibration_bytes*4;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&calibration_readback_)));
        hp.Type=D3D12_HEAP_TYPE_DEFAULT;d.Width=calibration_bytes;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&calibration_scratch_)));
    }
    for(unsigned type=0;type<2;++type){const auto kind=type?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandAllocator(kind,IID_PPV_ARGS(&allocators_[type])));
        for(unsigned i=0;i<4;++i){check(device->CreateCommandList(0,kind,allocators_[type].Get(),nullptr,IID_PPV_ARGS(&copies_[type][i])));
            auto* list=copies_[type][i].Get();
            if(timestamps_)list->EndQuery(timestamps_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2);
            copy_policy(list,buffer_.Get(),upload_.Get(),i*block_bytes);
            // Reset to an impossible witness (pipeline id zero) before every
            // controlled submission. A skipped/predicated shader cannot inherit
            // a marker from an earlier execution or uninitialized VRAM.
            barrier(list,marker_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            for(unsigned slot=0;slot<capacity;++slot){list->CopyBufferRegion(marker_.Get(),slot*marker_stride_,neutral_.Get(),0,32);if(spatial)list->CopyBufferRegion(marker_.Get(),slot*marker_stride_+32+2*spatial_page_bytes,model_upload_.Get(),i*model_block_bytes+slot*model_stride,sizeof(arc::SpatialModelTable));}
            barrier(list,marker_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if(timestamps_){list->EndQuery(timestamps_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2+1);list->ResolveQueryData(timestamps_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2,2,readback_.Get(),i*2*sizeof(UINT64));}
            check(list->Close());
            check(device->CreateCommandList(0,kind,allocators_[type].Get(),nullptr,IID_PPV_ARGS(&marker_copies_[type][i])));
            auto* marker_copy=marker_copies_[type][i].Get();
            barrier(marker_copy,marker_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            for(unsigned slot=0;slot<capacity;++slot)marker_copy->CopyBufferRegion(marker_readback_.Get(),i*marker_bytes+slot*32,marker_.Get(),slot*marker_stride_,32);
            if(spatial)marker_copy->CopyBufferRegion(spatial_readback_.Get(),i*spatial_page_bytes*2,marker_.Get(),32,spatial_page_bytes*2);
            barrier(marker_copy,marker_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);check(marker_copy->Close());
            if(calibration_queries_){check(device->CreateCommandList(0,kind,allocators_[type].Get(),nullptr,IID_PPV_ARGS(&calibration_copies_[type][i])));
                auto* copy=calibration_copies_[type][i].Get();copy->CopyBufferRegion(calibration_readback_.Get(),i*calibration_bytes,calibration_scratch_.Get(),0,calibration_bytes);check(copy->Close());}
        }
    }
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)));
    Ptr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    Ptr<ID3D12CommandAllocator> init_allocator;Ptr<ID3D12GraphicsCommandList> init;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&init_allocator)));
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,init_allocator.Get(),nullptr,IID_PPV_ARGS(&init)));
    Ptr<ID3D12Resource> init_zeros;
    if(spatial){D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;auto zero_desc=marker_->GetDesc();zero_desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        check(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&zero_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&init_zeros)));
        void* zeros{};check(init_zeros->Map(0,&none,&zeros));std::memset(zeros,0,SIZE_T(zero_desc.Width));init_zeros->Unmap(0,nullptr);
        barrier(init.Get(),marker_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);init->CopyResource(marker_.Get(),init_zeros.Get());barrier(init.Get(),marker_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    copy_policy(init.Get(),buffer_.Get(),neutral_.Get(),0);check(init->Close());ID3D12CommandList* lists[]{init.Get()};queue->ExecuteCommandLists(1,lists);
    auto quarantine=[&]{buffer_.Detach();neutral_.Detach();marker_.Detach();init_zeros.Detach();init_allocator.Detach();init.Detach();queue.Detach();fence_.Detach();};
    // Initialization is on the worker, never an intercepted application call.
    // On a signal failure, keep submitted storage alive until device removal.
    const HRESULT signaled=queue->Signal(fence_.Get(),++sequence_);
    if(FAILED(signaled)){quarantine();throw std::runtime_error("GPU control initialization signal failed");}
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event){quarantine();throw std::runtime_error("GPU control initialization event failed");}
    const auto registered=fence_->SetEventOnCompletion(sequence_,event);const auto waited=SUCCEEDED(registered)?WaitForSingleObject(event,5000):WAIT_FAILED;
    if(waited!=WAIT_OBJECT_0){if(FAILED(registered))CloseHandle(event);quarantine();throw std::runtime_error("GPU control initialization did not complete");}
    CloseHandle(event);
    for(auto* resource:{buffer_.Get(),marker_.Get(),calibration_scratch_.Get(),upload_.Get(),neutral_.Get(),model_upload_.Get(),readback_.Get(),marker_readback_.Get(),spatial_readback_.Get(),calibration_readback_.Get()})if(resource){D3D12_HEAP_PROPERTIES properties{};if(SUCCEEDED(resource->GetHeapProperties(&properties,nullptr))){const auto desc=resource->GetDesc();const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(bytes!=UINT64_MAX)allocation_bytes_[properties.Type==D3D12_HEAP_TYPE_UPLOAD?1:properties.Type==D3D12_HEAP_TYPE_READBACK?2:0]+=bytes;}}
}
D3D12_GPU_VIRTUAL_ADDRESS GpuControl::prepass_address(unsigned slot)const noexcept{return slot<capacity?buffer_->GetGPUVirtualAddress()+prepass_offset+slot*256:0;}
D3D12_GPU_VIRTUAL_ADDRESS GpuControl::address(unsigned slot)const noexcept{return slot<capacity?buffer_->GetGPUVirtualAddress()+slot*256:0;}
D3D12_GPU_VIRTUAL_ADDRESS GpuControl::marker_address(unsigned slot)const noexcept{return slot<capacity?marker_->GetGPUVirtualAddress()+slot*marker_stride_:0;}
void GpuControl::marker_barrier(ID3D12GraphicsCommandList* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=marker_.Get();list->ResourceBarrier(1,&b);}
bool GpuControl::ExecutionReadback::read(std::array<std::array<UINT64,2>,capacity>& actual,std::array<std::array<UINT,2>,capacity>* spatial)const noexcept{
    if(!resource||!fence||!completion)return false;const auto completed=fence->GetCompletedValue();if(completed==UINT64_MAX||completed<completion)return false;
    void* data{};D3D12_RANGE range{SIZE_T(offset),SIZE_T(offset+marker_bytes)};if(FAILED(resource->Map(0,&range,&data)))return false;
    for(unsigned i=0;i<capacity;++i){const auto* row=static_cast<const char*>(data)+offset+i*32;std::memcpy(actual[i].data(),row,16);if(spatial)std::memcpy((*spatial)[i].data(),row+16,8);}D3D12_RANGE empty{};resource->Unmap(0,&empty);return true;
}
std::optional<GpuControl::ExecutionReadback> GpuControl::execution_readback(ID3D12CommandQueue* queue)const{
    if(fault_||last_marker_slot_<0||last_queue_.Get()!=queue)return {};const auto slot=unsigned(last_marker_slot_);
    return ExecutionReadback{marker_leases_[slot],marker_readback_,fence_,retired_[slot],slot*marker_bytes,reinterpret_cast<UINT64>(queue),marker_expected_[slot]};
}
bool GpuControl::SpatialReadback::read(std::vector<SpatialTile>& tiles)const noexcept{
    static_assert(sizeof(SpatialTile)==spatial_record_bytes);
    if(!resource||!fence||!completion||!tile_width||!tile_height)return false;const auto done=fence->GetCompletedValue();if(done==UINT64_MAX||done<completion)return false;
    const UINT64 count=UINT64((width+tile_width-1)/tile_width)*((height+tile_height-1)/tile_height);if(!count||count>spatial_capacity)return false;
    try{tiles.resize(SIZE_T(count));void* data{};D3D12_RANGE range{SIZE_T(offset),SIZE_T(offset+count*spatial_record_bytes)};if(FAILED(resource->Map(0,&range,&data)))return false;
        std::memcpy(tiles.data(),static_cast<const char*>(data)+offset,SIZE_T(count*spatial_record_bytes));for(auto& tile:tiles)tile.mode&=255;D3D12_RANGE empty{};resource->Unmap(0,&empty);return true;
    }catch(...){return false;}
}
void GpuControl::calibration_mark(ID3D12GraphicsCommandList* list,unsigned slot,unsigned point){
    if(!calibration_queries_||slot>=capacity||point>3)throw std::runtime_error("Calibration query capacity");
    if(!point){if(!calibration_mask_)barrier(list,calibration_scratch_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);calibration_mask_|=1u<<slot;}
    list->EndQuery(calibration_queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4+point);
}
void GpuControl::calibration_predicate(ID3D12GraphicsCommandList* list,unsigned slot){
    static_assert(offsetof(ControlValue,calibration)%8==0);
    // DX12 predicates OUT the operation when the comparison is true.
    list->SetPredication(buffer_.Get(),slot*256+offsetof(ControlValue,calibration),D3D12_PREDICATION_OP_EQUAL_ZERO);
}
void GpuControl::record_neutralize(ID3D12GraphicsCommandList* list){
    // CopyBufferRegion is predicable. At the end of this recording, no later
    // application command can depend on preserving its predication state.
    list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    if(calibration_mask_)list->EndQuery(calibration_queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,capacity*4);
    copy_policy(list,buffer_.Get(),neutral_.Get(),0);
    if(calibration_mask_){
        list->EndQuery(calibration_queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,capacity*4+1);
        for(unsigned i=0;i<capacity;++i)if(calibration_mask_&(1u<<i))list->ResolveQueryData(calibration_queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*4,4,calibration_scratch_.Get(),i*4*sizeof(UINT64));
        list->ResolveQueryData(calibration_queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,capacity*4,2,calibration_scratch_.Get(),capacity*4*sizeof(UINT64));
        barrier(list,calibration_scratch_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
}
D3D12_GPU_VIRTUAL_ADDRESS GpuControl::probe_address(unsigned slot,bool candidate)const noexcept{return buffer_->GetGPUVirtualAddress()+prepass_offset*(candidate?3:2)+UINT64(slot)*256;}
void GpuControl::shadow_reuse_predicate(ID3D12GraphicsCommandList* list,unsigned slot){if(slot>=capacity)throw std::out_of_range("shadow predicate slot");list->SetPredication(buffer_.Get(),UINT64(slot)*256+offsetof(ControlValue,reserved),D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);}
void GpuControl::probe_predicate(ID3D12GraphicsCommandList* list,unsigned slot){list->SetPredication(buffer_.Get(),UINT64(slot)*256+offsetof(ControlValue,probe_epoch),D3D12_PREDICATION_OP_EQUAL_ZERO);}
void GpuControl::model(unsigned slot,const arc::SpatialModelTable& table){if(models_&&slot<capacity){auto& current=(*models_)[slot];if(current.key!=table.key||current.reserved!=table.reserved)current=table;}}
ID3D12CommandList* GpuControl::prepare(ID3D12CommandQueue* queue,std::span<const ControlValue> values,std::span<const ControlValue> probes){
    pending_=-1;
    if(fault_)throw std::runtime_error("Faulted GPU control");
    const auto kind=queue->GetDesc().Type;
    if(kind!=D3D12_COMMAND_LIST_TYPE_DIRECT&&kind!=D3D12_COMMAND_LIST_TYPE_COMPUTE)throw std::runtime_error("Unsupported control queue");
    const auto completed=fence_->GetCompletedValue();
    if(last_queue_&&last_queue_.Get()!=queue&&completed<sequence_)check(queue->Wait(fence_.Get(),sequence_));
    if(values.size()>capacity)throw std::runtime_error("GPU control capacity");
    bool coarse=false;for(const auto& value:values)coarse|=value.reserved==1||(value.sample_percent>=1&&value.sample_percent<100)||value.probe_epoch!=0||value.calibration!=0||(value.x==2||value.x==4)||(value.y==2||value.y==4)||(value.comparison_taps>=1&&value.comparison_taps<25)||value.zero_factor==1||(value.mip_steps>=1&&value.mip_steps<=8)||(value.edge_sources&&value.spatial_key);
    if(!coarse)return nullptr;
    // Prefer completed slots already consumed by the background collector.
    // Fall back to overwriting an unread slot rather than delaying the game.
    for(unsigned i=0;i<4;++i)if(retired_[i]<=completed&&!unread_[i]&&marker_leases_[i].use_count()==1){pending_=static_cast<int>(i);break;}
    if(pending_<0)for(unsigned i=0;i<4;++i)if(retired_[i]<=completed&&marker_leases_[i].use_count()==1){pending_=static_cast<int>(i);break;}
    if(pending_<0)return nullptr; // previous recording's GPU epilogue is neutral
    calibration_epochs_[pending_]={};calibration_pipelines_[pending_]={};for(unsigned i=0;i<values.size();++i)if(calibration_mask_&(1u<<i)){calibration_epochs_[pending_][i]=values[i].calibration;calibration_pipelines_[pending_][i]=values[i].calibration_pipeline;}
    marker_expected_[pending_]={};for(unsigned i=0;i<values.size();++i)marker_expected_[pending_][i]={values[i].proof_epoch,values[i].proof_pipeline};
    if(timestamps_){
        // Slow collectors must not stall application submissions or read data
        // that a reused ring slot is about to overwrite.
        if(unread_[pending_]){++timing_.dropped;unread_[pending_]=false;}
        pending_frequency_=0;
        if(FAILED(queue->GetTimestampFrequency(&pending_frequency_)))pending_frequency_=0;
    }
    if(model_upload_){void* memory{};D3D12_RANGE none{};check(model_upload_->Map(0,&none,&memory));for(unsigned i=0;i<capacity;++i)std::memcpy(static_cast<char*>(memory)+pending_*model_block_bytes+i*model_stride,&(*models_)[i],sizeof(arc::SpatialModelTable));model_upload_->Unmap(0,nullptr);}
    void* data{};D3D12_RANGE none{};check(upload_->Map(0,&none,&data));
    auto* block=static_cast<char*>(data)+pending_*block_bytes;std::memset(block,0,block_bytes);
    for(unsigned i=0;i<capacity;++i){ControlValue value=i<values.size()?values[i]:ControlValue{};
        if(marker_stride_>32&&value.edge_sources&&value.spatial_tile_width&&value.spatial_tile_height&&sequence_<UINT_MAX-1){
            const UINT64 tiles=UINT64((value.width+value.spatial_tile_width-1)/value.spatial_tile_width)*((value.height+value.spatial_tile_height-1)/value.spatial_tile_height);
            if(tiles&&tiles<=spatial_capacity){value.spatial_capacity=spatial_capacity;if(!value.spatial_frame)value.spatial_frame=UINT(sequence_+1);value.spatial_flags=(value.spatial_flags&~1u)|UINT((sequence_+1)&1);}
        }
        if(i==0)spatial_values_[pending_]=value;
        std::memcpy(block+i*256,&value,sizeof(value));
        auto probe=i<probes.size()?probes[i]:value;probe.probe_epoch=value.probe_epoch;probe.probe_stride=value.probe_stride;probe.probe_phase=value.probe_phase;probe.spatial_capacity=value.spatial_capacity;probe.spatial_frame=value.spatial_frame;probe.spatial_key=value.spatial_key;probe.spatial_flags=value.spatial_flags&~16u;probe.proof_epoch=probe.proof_pipeline=0;probe.edge_sources=0;
        std::memcpy(block+prepass_offset*3+i*256,&probe,sizeof(probe));probe.x=probe.y=1;probe.sample_percent=100;probe.comparison_taps=probe.zero_factor=probe.mip_steps=0;std::memcpy(block+prepass_offset*2+i*256,&probe,sizeof(probe));
        value.proof_epoch=value.proof_pipeline=0;value.spatial_flags|=16;std::memcpy(block+prepass_offset+i*256,&value,sizeof(value));}
    upload_->Unmap(0,nullptr);
    return copies_[kind==D3D12_COMMAND_LIST_TYPE_COMPUTE?1:0][pending_].Get();
}
void GpuControl::submitted(ID3D12CommandQueue* queue){
    last_queue_=queue;
    last_marker_slot_=-1;
    const bool proof=pending_>=0&&std::any_of(marker_expected_[pending_].begin(),marker_expected_[pending_].end(),[](const auto& value){return value[0]!=0;});
    const bool map=pending_>=0&&spatial_readback_&&spatial_values_[pending_].spatial_capacity&&(proof||(spatial_values_[pending_].spatial_flags&8));
    if(proof||map){
        ID3D12CommandList* list=marker_copies_[queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_COMPUTE?1:0][pending_].Get();queue->ExecuteCommandLists(1,&list);if(proof)last_marker_slot_=pending_;
    }
    if(pending_>=0&&std::any_of(calibration_epochs_[pending_].begin(),calibration_epochs_[pending_].end(),[](auto value){return value!=0;})){
        ID3D12CommandList* list=calibration_copies_[queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_COMPUTE?1:0][pending_].Get();queue->ExecuteCommandLists(1,&list);
    }
    const auto value=++sequence_;
    if(FAILED(queue->Signal(fence_.Get(),value))){fault_=true;throw std::runtime_error("GPU control retirement signal failed");}
    if(map){const auto& control=spatial_values_[pending_];latest_spatial_=SpatialReadback{marker_leases_[pending_],spatial_readback_,fence_,value,UINT64(pending_)*spatial_page_bytes*2+(control.spatial_flags&1)*spatial_page_bytes,control.reserved,control.spatial_key,reinterpret_cast<UINT64>(queue),control.width,control.height,control.spatial_tile_width,control.spatial_tile_height,control.spatial_frame,control.edge_threshold,(control.spatial_flags&4)!=0,control.spatial_center>0,(control.spatial_flags&32)!=0,control.model_limit,control.x,control.y,control.mip_steps,control.sample_percent};}
    if(pending_>=0){retired_[pending_]=value;if(timestamps_){frequencies_[pending_]=pending_frequency_;unread_[pending_]=true;}}pending_=-1;
}
void GpuControl::collect_timing()noexcept{
    if(!timestamps_||fault_)return;
    const auto completed=fence_->GetCompletedValue();
    if(completed==UINT64_MAX)return; // device removal is not completed evidence
    for(unsigned i=0;i<4;++i){
        if(!unread_[i]||retired_[i]>completed)continue;
        unread_[i]=false;
        D3D12_RANGE range{i*2*sizeof(UINT64),(i*2+2)*sizeof(UINT64)};void* data{};
        if(FAILED(readback_->Map(0,&range,&data))){++timing_.invalid;continue;}
        UINT64 ticks[2];std::memcpy(ticks,static_cast<const char*>(data)+range.Begin,sizeof(ticks));
        D3D12_RANGE none{};readback_->Unmap(0,&none);
        if(!frequencies_[i]||ticks[1]<ticks[0]){++timing_.invalid;continue;}
        timing_.milliseconds+=double(ticks[1]-ticks[0])*1000.0/double(frequencies_[i]);++timing_.samples;
        const auto& epochs=calibration_epochs_[i];
        if(std::any_of(epochs.begin(),epochs.end(),[](auto value){return value!=0;})){
            D3D12_RANGE calibration_range{SIZE_T(i*calibration_bytes),SIZE_T((i+1)*calibration_bytes)};void* samples{};
            if(FAILED(calibration_readback_->Map(0,&calibration_range,&samples))){++timing_.invalid;continue;}
            const auto* values=reinterpret_cast<const UINT64*>(static_cast<const char*>(samples)+calibration_range.Begin);
            const auto scale=1000.0/double(frequencies_[i]);
            for(unsigned slot=0;slot<capacity;++slot)if(epochs[slot]){
                const auto* q=values+slot*4;
                if(q[1]<q[0]||q[3]<q[2]||values[capacity*4+1]<values[capacity*4]){++timing_.invalid;continue;}
                if(calibrations_.size()==512)calibrations_.erase(calibrations_.begin());
                calibrations_.push_back({epochs[slot],calibration_pipelines_[i][slot],slot,(q[1]-q[0])*scale,(q[3]-q[2])*scale,(values[capacity*4+1]-values[capacity*4])*scale,(ticks[1]-ticks[0])*scale});
            }
            calibration_readback_->Unmap(0,&none);
        }
    }
}
bool GpuControl::ready()const noexcept{const auto completed=fence_->GetCompletedValue();return !fault_&&pending_<0&&completed!=UINT64_MAX&&completed>=sequence_;}
void GpuControl::release_completed_queue()noexcept{if(ready()&&last_marker_slot_<0)last_queue_.Reset();}
}
