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
    // An explicit calibration may execute extra work; cached replay with a
    // zero/retired policy must NOT do so. Test the GPU predicate with actual
    // copied bytes, independently of timestamp/provenance bookkeeping.
    GpuControl calibration(device.Get(),true);calibration.begin_recording();
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=16;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> source,output;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source)));
    hp.Type=D3D12_HEAP_TYPE_READBACK;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&output)));
    void* mapped{};D3D12_RANGE empty{};hr(source->Map(0,&empty,&mapped));static_cast<UINT64*>(mapped)[0]=0;static_cast<UINT64*>(mapped)[1]=0x12345678;source->Unmap(0,nullptr);
    ComPtr<ID3D12CommandAllocator> ca;ComPtr<ID3D12GraphicsCommandList> cl;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&ca)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,ca.Get(),nullptr,IID_PPV_ARGS(&cl)));
    cl->CopyBufferRegion(output.Get(),0,source.Get(),0,8);
    calibration.calibration_mark(cl.Get(),0,0);calibration.calibration_mark(cl.Get(),0,1);
    calibration.calibration_predicate(cl.Get(),0);calibration.calibration_mark(cl.Get(),0,2);
    cl->CopyBufferRegion(output.Get(),0,source.Get(),8,8);calibration.calibration_mark(cl.Get(),0,3);
    cl->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);calibration.record_neutralize(cl.Get());hr(cl->Close());
    for(UINT64 epoch:{UINT64(0),UINT64(42),UINT64(0)}){
        arc::dx12::optimizer::ControlValue cv;cv.calibration=epoch;cv.calibration_pipeline=777;
        if(auto* helper=calibration.prepare(direct.Get(),{&cv,1}))direct->ExecuteCommandLists(1,&helper);
        ID3D12CommandList* work[]{cl.Get()};direct->ExecuteCommandLists(1,work);calibration.submitted(direct.Get());wait(direct.Get());calibration.collect_timing();
        D3D12_RANGE read{0,8};hr(output->Map(0,&read,&mapped));const auto actual=*static_cast<const UINT64*>(mapped);output->Unmap(0,&empty);
        check(actual==(epoch?0x12345678u:0u),"calibration GPU predicate and cached neutral replay");
    }
    check(calibration.calibrations().size()==1&&calibration.calibrations().front().epoch==42&&calibration.calibrations().front().pipeline==777,"only executed calibration produces correctly identified evidence");
    for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T bytes{};hr(diagnostics->GetMessage(i,nullptr,&bytes));std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(diagnostics->GetMessage(i,message,&bytes));check(message->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"D3D12 debug error");}
    CloseHandle(event);std::cout<<"GPU control timing passed: samples="<<result.samples<<" dropped="<<result.dropped<<" gpu_ms="<<result.milliseconds<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
