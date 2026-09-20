#include "generic_gpu_control.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace arc::dx12::optimizer {
namespace {
constexpr UINT64 block_bytes=GpuControl::capacity*D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
void check(HRESULT result){if(FAILED(result))throw std::runtime_error("GPU control HRESULT "+std::to_string(result));}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
void copy_policy(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,ID3D12Resource* source,UINT64 offset){
    barrier(list,buffer,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(buffer,0,source,offset,block_bytes);
    barrier(list,buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}
}
GpuControl::GpuControl(ID3D12Device* device):device_(device){
    if(!device||device->GetNodeCount()!=1)throw std::runtime_error("GPU control requires one node");
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=block_bytes;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,nullptr,IID_PPV_ARGS(&buffer_)));
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&neutral_)));
    d.Width*=4;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload_)));
    void* data{};D3D12_RANGE none{};check(neutral_->Map(0,&none,&data));std::memset(data,0,block_bytes);
    for(unsigned i=0;i<capacity;++i){const ControlValue neutral;std::memcpy(static_cast<char*>(data)+i*256,&neutral,sizeof(neutral));}neutral_->Unmap(0,nullptr);
    for(unsigned type=0;type<2;++type){const auto kind=type?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandAllocator(kind,IID_PPV_ARGS(&allocators_[type])));
        for(unsigned i=0;i<4;++i){check(device->CreateCommandList(0,kind,allocators_[type].Get(),nullptr,IID_PPV_ARGS(&copies_[type][i])));
            copy_policy(copies_[type][i].Get(),buffer_.Get(),upload_.Get(),i*block_bytes);check(copies_[type][i]->Close());}
    }
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)));
    Ptr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    Ptr<ID3D12CommandAllocator> init_allocator;Ptr<ID3D12GraphicsCommandList> init;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&init_allocator)));
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,init_allocator.Get(),nullptr,IID_PPV_ARGS(&init)));
    copy_policy(init.Get(),buffer_.Get(),neutral_.Get(),0);check(init->Close());ID3D12CommandList* lists[]{init.Get()};queue->ExecuteCommandLists(1,lists);
    // Initialization is on the worker, never an intercepted application call.
    // On a signal failure, keep submitted storage alive until device removal.
    const HRESULT signaled=queue->Signal(fence_.Get(),++sequence_);
    if(FAILED(signaled)){buffer_.Detach();neutral_.Detach();init_allocator.Detach();init.Detach();queue.Detach();fence_.Detach();throw std::runtime_error("GPU control initialization signal failed");}
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event){buffer_.Detach();neutral_.Detach();init_allocator.Detach();init.Detach();queue.Detach();fence_.Detach();throw std::runtime_error("GPU control initialization event failed");}
    const auto registered=fence_->SetEventOnCompletion(sequence_,event);const auto waited=SUCCEEDED(registered)?WaitForSingleObject(event,5000):WAIT_FAILED;
    if(waited!=WAIT_OBJECT_0){if(FAILED(registered))CloseHandle(event);buffer_.Detach();neutral_.Detach();init_allocator.Detach();init.Detach();queue.Detach();fence_.Detach();throw std::runtime_error("GPU control initialization did not complete");}
    CloseHandle(event);
}
D3D12_GPU_VIRTUAL_ADDRESS GpuControl::address(unsigned slot)const noexcept{return slot<capacity?buffer_->GetGPUVirtualAddress()+slot*256:0;}
void GpuControl::record_neutralize(ID3D12GraphicsCommandList* list){
    // CopyBufferRegion is predicable. At the end of this recording, no later
    // application command can depend on preserving its predication state.
    list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    copy_policy(list,buffer_.Get(),neutral_.Get(),0);
}
ID3D12CommandList* GpuControl::prepare(ID3D12CommandQueue* queue,std::span<const ControlValue> values){
    pending_=-1;
    if(fault_)throw std::runtime_error("Faulted GPU control");
    const auto kind=queue->GetDesc().Type;
    if(kind!=D3D12_COMMAND_LIST_TYPE_DIRECT&&kind!=D3D12_COMMAND_LIST_TYPE_COMPUTE)throw std::runtime_error("Unsupported control queue");
    const auto completed=fence_->GetCompletedValue();
    if(last_queue_&&last_queue_.Get()!=queue&&completed<sequence_)check(queue->Wait(fence_.Get(),sequence_));
    if(values.size()>capacity)throw std::runtime_error("GPU control capacity");
    bool coarse=false;for(const auto& value:values)coarse|=value.x==2||value.y==2||value.comparison_taps==9||value.zero_factor==1;
    if(!coarse)return nullptr;
    for(unsigned i=0;i<4;++i)if(retired_[i]<=completed){pending_=static_cast<int>(i);break;}
    if(pending_<0)return nullptr; // previous recording's GPU epilogue is neutral
    void* data{};D3D12_RANGE none{};check(upload_->Map(0,&none,&data));
    auto* block=static_cast<char*>(data)+pending_*block_bytes;std::memset(block,0,block_bytes);
    for(unsigned i=0;i<capacity;++i){const ControlValue value=i<values.size()?values[i]:ControlValue{};std::memcpy(block+i*256,&value,sizeof(value));}
    upload_->Unmap(0,nullptr);
    return copies_[kind==D3D12_COMMAND_LIST_TYPE_COMPUTE?1:0][pending_].Get();
}
void GpuControl::submitted(ID3D12CommandQueue* queue){
    last_queue_=queue;
    const auto value=++sequence_;
    if(FAILED(queue->Signal(fence_.Get(),value))){fault_=true;throw std::runtime_error("GPU control retirement signal failed");}
    if(pending_>=0)retired_[pending_]=value;pending_=-1;
}
bool GpuControl::ready()const noexcept{return !fault_&&fence_->GetCompletedValue()>=sequence_;}
void GpuControl::release_completed_queue()noexcept{if(ready())last_queue_.Reset();}
}
