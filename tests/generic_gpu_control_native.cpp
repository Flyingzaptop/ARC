#include "generic_gpu_control.hpp"
#include <d3d12sdklayers.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using arc::dx12::optimizer::GpuControl;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void hr(HRESULT value){check(SUCCEEDED(value),"D3D12 call failed");}
int main()try{
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> diagnostics;hr(device.As(&diagnostics));
    ComPtr<ID3D12CommandQueue> direct,compute;D3D12_COMMAND_QUEUE_DESC qd{};
    hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&direct)));qd.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&compute)));
    ComPtr<ID3D12Fence> fence,gate;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    UINT64 serial{};HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");
    auto wait=[&](ID3D12CommandQueue* queue){hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"bounded GPU completion");};
    GpuControl control(device.Get(),true);arc::dx12::optimizer::ControlValue value{2,2,64,64};
    auto submit=[&](ID3D12CommandQueue* queue){auto* list=control.prepare(queue,{&value,1});check(list!=nullptr,"active helper available");check(!control.ready(),"prepared policy cannot be retired before submission signal");queue->ExecuteCommandLists(1,&list);control.submitted(queue);};
    hr(direct->Wait(gate.Get(),1));submit(direct.Get());control.collect_timing();
    const auto pending=control.timing();hr(gate->Signal(1));wait(direct.Get());
    check(pending.samples==0&&pending.invalid==0,"pending GPU data cannot be consumed");
    control.collect_timing();auto first=control.timing();check(first.samples==1&&first.invalid==0,"one completed direct sample");
    control.collect_timing();check(control.timing().samples==1,"no double counting");
    submit(compute.Get());wait(compute.Get());control.collect_timing();check(control.timing().samples==2,"compute queue frequency and readback");
    // Completed, unread slots may be reused without waiting for the collector.
    for(unsigned i=0;i<5;++i){auto* queue=i%2?compute.Get():direct.Get();submit(queue);wait(queue);}control.collect_timing();
    auto result=control.timing();check(result.samples==6&&result.dropped==1&&result.invalid==0,"reused sample explicitly dropped");
    check(std::isfinite(result.milliseconds)&&result.milliseconds>=0,"finite GPU duration");
    check(control.prepare(direct.Get(),{})==nullptr,"neutral has no upload helper");control.submitted(direct.Get());wait(direct.Get());control.collect_timing();
    check(control.timing().samples==result.samples,"neutral retirement does not manufacture samples");
    GpuControl disabled(device.Get());auto* list=disabled.prepare(direct.Get(),{&value,1});direct->ExecuteCommandLists(1,&list);disabled.submitted(direct.Get());wait(direct.Get());disabled.collect_timing();check(disabled.timing().samples==0,"disabled measurement stays off");
    for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T bytes{};hr(diagnostics->GetMessage(i,nullptr,&bytes));std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(diagnostics->GetMessage(i,message,&bytes));check(message->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"D3D12 debug error");}
    CloseHandle(event);std::cout<<"GPU control timing passed: samples="<<result.samples<<" dropped="<<result.dropped<<" gpu_ms="<<result.milliseconds<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
