#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void hr(HRESULT value){if(FAILED(value))throw std::runtime_error("HRESULT "+std::to_string(value));}
std::string read(const std::filesystem::path& path){std::ifstream file(path);return {(std::istreambuf_iterator<char>(file)),{}};}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"DLL and fresh evidence directory required");const auto directory=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(directory),"Fresh directory required");std::filesystem::create_directories(directory);
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    HMODULE dll=LoadLibraryW(argv[1]);check(dll!=nullptr,"Load ARC");using Api=DWORD(WINAPI*)(void*);
    auto init=reinterpret_cast<Api>(GetProcAddress(dll,"ArcInitialize"));auto lean=reinterpret_cast<Api>(GetProcAddress(dll,"ArcUseLeanMode"));
    auto request=reinterpret_cast<Api>(GetProcAddress(dll,"ArcRequestGpuProfile"));auto stop=reinterpret_cast<Api>(GetProcAddress(dll,"ArcStopGpuProfile"));
    auto runtime=(directory/L"runtime.json").wstring();check(init&&lean&&request&&stop&&init(runtime.data())==0&&lean(nullptr)==0,"Initialize lean observer");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> diagnostics;hr(device.As(&diagnostics));
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue,other;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)));
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;
    ComPtr<ID3DBlob> blob,error;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char* source=R"(
    RWStructuredBuffer<uint> data : register(u0);
    [numthreads(64,1,1)] void first(uint3 id:SV_DispatchThreadID){data[id.x]=id.x*3+7;}
    [numthreads(128,1,1)] void second(uint3 id:SV_DispatchThreadID){data[id.x]=data[id.x]^0x13579BDF;}
    )";
    ComPtr<ID3D12PipelineState> a,b;
    auto make_pipeline=[&](const char* entry,ComPtr<ID3D12PipelineState>& pipeline){ComPtr<ID3DBlob> code;hr(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error));D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={code->GetBufferPointer(),code->GetBufferSize()};hr(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline)));};
    make_pipeline("first",a);make_pipeline("second",b);
    D3D12_INDIRECT_ARGUMENT_DESC indirect_desc{};indirect_desc.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS),1,&indirect_desc,0};ComPtr<ID3D12CommandSignature> indirect_signature;
    hr(device->CreateCommandSignature(&signature_desc,nullptr,IID_PPV_ARGS(&indirect_signature)));
    D3D12_HEAP_PROPERTIES upload_hp{};upload_hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC args_desc{};args_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;args_desc.Width=16;args_desc.Height=args_desc.DepthOrArraySize=args_desc.MipLevels=args_desc.SampleDesc.Count=1;args_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> indirect_arguments;hr(device->CreateCommittedResource(&upload_hp,D3D12_HEAP_FLAG_NONE,&args_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indirect_arguments)));
    void* args_data{};D3D12_RANGE no_read{};hr(indirect_arguments->Map(0,&no_read,&args_data));*static_cast<D3D12_DISPATCH_ARGUMENTS*>(args_data)={128,1,1};indirect_arguments->Unmap(0,nullptr);

    constexpr UINT count=16384;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=count*4;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> data,readback;
    hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&data)));
    bd.Flags=D3D12_RESOURCE_FLAG_NONE;hp.Type=D3D12_HEAP_TYPE_READBACK;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),a.Get(),IID_PPV_ARGS(&commands)));hr(commands->Close());
    auto record=[&]{hr(allocator->Reset());hr(commands->Reset(allocator.Get(),a.Get()));commands->SetComputeRootSignature(root.Get());commands->SetComputeRootUnorderedAccessView(0,data->GetGPUVirtualAddress());commands->Dispatch(count/64,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=data.Get();commands->ResourceBarrier(1,&barrier);commands->SetPipelineState(b.Get());commands->ExecuteIndirect(indirect_signature.Get(),1,indirect_arguments.Get(),0,nullptr,0);
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={data.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};commands->ResourceBarrier(1,&barrier);commands->CopyBufferRegion(readback.Get(),0,data.Get(),0,count*4);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);hr(commands->Close());};
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 value=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Fence event");
    auto wait=[&](ID3D12CommandQueue* q){hr(q->Signal(fence.Get(),++value));hr(fence->SetEventOnCompletion(value,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU wait timeout");};
    ID3D12CommandList* lists[]{commands.Get()};
    auto verify=[&]{void* ptr{};D3D12_RANGE range{0,count*4},empty{};hr(readback->Map(0,&range,&ptr));for(UINT i=0;i<count;++i)check(static_cast<UINT*>(ptr)[i]==((i*3+7)^0x13579BDF),"Profiling must preserve every compute output exactly");readback->Unmap(0,&empty);};
    record();queue->ExecuteCommandLists(1,lists);wait(queue.Get());verify();
    auto output=(directory/L"profile.json").wstring();auto argument=L"1|"+output;check(request(argument.data())==0,"Bounded GPU profile request");check(request(argument.data())!=0,"Overlapping profile refused");
    auto passive=reinterpret_cast<Api>(GetProcAddress(dll,"ArcUsePassiveMode"));auto mode=reinterpret_cast<Api>(GetProcAddress(dll,"ArcExperimentalVrs"));wchar_t coarse[]=L"2x2";
    check(passive&&mode&&passive(nullptr)!=0&&mode(coarse)!=0,"Hook removal and VRS must not interrupt an active cost capture");
    record();for(int i=0;i<32;++i){queue->ExecuteCommandLists(1,lists);wait(queue.Get());verify();}
    // The runtime may intern immutable PSOs and return the same object again.
    // Existing recordings and future recordings must keep one profile identity.
    ComPtr<ID3D12PipelineState> duplicate;make_pipeline("first",duplicate);
    // Reset may start a new GPU generation while a previous generation is
    // waiting. The thread that submits must still be able to release the gate.
    ComPtr<ID3D12Fence> gate;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));hr(queue->Wait(gate.Get(),1));queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),++value));
    auto previous_allocator=allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));record();
    hr(other->Wait(fence.Get(),value));other->ExecuteCommandLists(1,lists);hr(gate->Signal(1));wait(other.Get());verify();
    D3D12_COMMAND_QUEUE_DESC cq{};cq.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;ComPtr<ID3D12CommandQueue> compute_queue;hr(device->CreateCommandQueue(&cq,IID_PPV_ARGS(&compute_queue)));
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COMPUTE,allocator.Get(),a.Get(),IID_PPV_ARGS(commands.ReleaseAndGetAddressOf())));hr(commands->Close());lists[0]=commands.Get();record();compute_queue->ExecuteCommandLists(1,lists);wait(compute_queue.Get());verify();
    check(stop(nullptr)==0,"Stop capture");for(int i=0;i<500&&!std::filesystem::exists(output);++i)Sleep(10);
    auto report=read(output);check(report.find("\"faults\":0")!=std::string::npos,"Healthy GPU profile");check(report.find("\"pending_gpu_jobs\":0")!=std::string::npos,"Read only completed query data");
    check(report.find("\"threads\":[64,1,1]")!=std::string::npos&&report.find("\"threads\":[128,1,1]")!=std::string::npos,"Independently reflected both compute kernels");
    check(report.find("\"indirect_calls\":1")!=std::string::npos,"Indirect dispatch is timed without guessing GPU-generated dimensions");
    check(report.find("\"kind\":\"compute\"")!=std::string::npos&&report.find("\"shader_mutations\":0")!=std::string::npos,"Compute costs without shader changes");check(request(argument.data())!=0,"Existing evidence cannot be overwritten");
    const auto pipeline_section=report.find("\"pipelines\":[");check(pipeline_section!=std::string::npos,"Pipeline inventory");unsigned identities=0;auto cursor=pipeline_section;
    while((cursor=report.find("\"id\":",cursor))!=std::string::npos){++identities;cursor+=5;}check(identities==2,"Recreated immutable PSO must not split existing profile identity");
    // A cached instrumented list still owns valid storage after report export.
    compute_queue->ExecuteCommandLists(1,lists);wait(compute_queue.Get());verify();commands.Reset();allocator.Reset();previous_allocator.Reset();
    auto abandoned_output=(directory/L"abandoned.json").wstring();auto abandoned_argument=L"1|"+abandoned_output;check(request(abandoned_argument.data())==0,"Start unfinished-recording regression capture");
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),a.Get(),IID_PPV_ARGS(&commands)));
    commands->SetComputeRootSignature(root.Get());commands->SetComputeRootUnorderedAccessView(0,data->GetGPUVirtualAddress());commands->Dispatch(count/64,1,1);
    check(stop(nullptr)==0,"Stop with application-owned open recording");
    for(int i=0;i<650&&!std::filesystem::exists(abandoned_output);++i)Sleep(10);
    check(std::filesystem::exists(abandoned_output),"Abandoned recording capture must publish");
    check(passive(nullptr)==0,"Open application list must not block passive mode forever");
    // ARC cannot close the application's list or release its timestamp heap.
    hr(commands->Close());lists[0]=commands.Get();queue->ExecuteCommandLists(1,lists);wait(queue.Get());
    commands.Reset();allocator.Reset();
    for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T bytes=0;diagnostics->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(diagnostics->GetMessage(i,message,&bytes));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 validation error");}}
    CloseHandle(event);hr(device->GetDeviceRemovedReason());std::cout<<"GPU profile: exact compute outputs, reflection, cached replay, in-flight Reset, two queues and post-export lifetime PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
