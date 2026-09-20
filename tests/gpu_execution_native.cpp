#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "generic_shader_transform.hpp"
#include "generic_binding_state.hpp"
#include "generic_gpu_control.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <filesystem>
using Microsoft::WRL::ComPtr;
using arc::dx12::optimizer::GpuControl;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void hr(HRESULT value){check(SUCCEEDED(value),"D3D12/DXC failure");}
ComPtr<IDxcBlob> result(IDxcOperationResult* op){HRESULT status{};hr(op->GetStatus(&status));if(FAILED(status)){ComPtr<IDxcBlobEncoding> errors;op->GetErrorBuffer(&errors);if(errors)std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());}hr(status);ComPtr<IDxcBlob> blob;hr(op->GetResult(&blob));return blob;}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"Absolute DXC DLL required");const std::filesystem::path compiler_path=argv[1];
    auto validator_module=LoadLibraryExW((compiler_path.parent_path()/L"dxil.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto compiler_module=LoadLibraryExW(compiler_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);check(compiler_module&&validator_module,"Load compiler");
    auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler_module,"DxcCreateInstance"));auto create_validator=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator_module,"DxcCreateInstance"));
    ComPtr<IDxcLibrary> library;ComPtr<IDxcCompiler> compiler;ComPtr<IDxcAssembler> assembler;ComPtr<IDxcValidator> validator;
    hr(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)));hr(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));hr(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler)));hr(create_validator(CLSID_DxcValidator,IID_PPV_ARGS(&validator)));
    const char source[]="RWTexture2D<float4> Output:register(u0); [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){Output[p.xy]=float4(p.x,p.y,0,1);}";
    ComPtr<IDxcBlobEncoding> text;hr(library->CreateBlobWithEncodingOnHeapCopy(source,sizeof(source)-1,CP_UTF8,&text));ComPtr<IDxcOperationResult> op;hr(compiler->Compile(text.Get(),L"marker",L"main",L"cs_6_0",nullptr,0,nullptr,0,nullptr,&op));auto original=result(op.Get());
    ComPtr<IDxcBlobEncoding> ir;hr(compiler->Disassemble(original.Get(),&ir));const auto transformed=arc::dx12::shader::coarse_compute({static_cast<const char*>(ir->GetBufferPointer()),ir->GetBufferSize()},1,1,true,1,true);
    check(transformed.admitted&&transformed.execution_marker,"Marker transform admission");text.Reset();hr(library->CreateBlobWithEncodingOnHeapCopy(transformed.ir.data(),UINT(transformed.ir.size()),CP_UTF8,&text));op.Reset();hr(assembler->AssembleToContainer(text.Get(),&op));auto code=result(op.Get());op.Reset();hr(validator->Validate(code.Get(),DxcValidatorFlags_InPlaceEdit,&op));code=result(op.Get());
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));ComPtr<ID3D12InfoQueue> diagnostics;hr(device.As(&diagnostics));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};D3D12_ROOT_SIGNATURE_DESC rd{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> root_blob;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));
    auto bytes=arc::dx12::binding::append_control_cbv({static_cast<const std::byte*>(root_blob->GetBufferPointer()),root_blob->GetBufferSize()},1,true);check(!bytes.empty(),"Root marker extension");ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,bytes.data(),bytes.size(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};ComPtr<ID3D12PipelineState> pipeline;hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=16;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));device->CreateUnorderedAccessView(output.Get(),nullptr,nullptr,heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=8;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> predicate;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&predicate)));void* mapped{};D3D12_RANGE empty{};hr(predicate->Map(0,&empty,&mapped));std::memset(mapped,0,8);predicate->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue,other;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)));
    GpuControl control(device.Get());ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<ID3D12Fence> done;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial{};
    std::vector<GpuControl::ExecutionReadback> held;
    for(unsigned iteration=0;iteration<4;++iteration){hr(allocator->Reset());hr(list->Reset(allocator.Get(),pipeline.Get()));list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,control.address(0));list->SetComputeRootUnorderedAccessView(2,control.marker_address(0));
        if(iteration==1)list->SetPredication(predicate.Get(),0,D3D12_PREDICATION_OP_EQUAL_ZERO);
        list->Dispatch(2,2,1);list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);control.marker_barrier(list.Get());control.record_neutralize(list.Get());hr(list->Close());
        arc::dx12::optimizer::ControlValue value{2,1,16,16};value.proof_epoch=UINT64(0x1234567800000000)+iteration+1;value.proof_pipeline=UINT64(0x8765432100000042);
        auto* helper=control.prepare(queue.Get(),{&value,1});check(helper,"Free proof ring slot");queue->ExecuteCommandLists(1,&helper);ID3D12CommandList* work[]{list.Get()};queue->ExecuteCommandLists(1,work);control.submitted(queue.Get());hr(queue->Signal(done.Get(),++serial));hr(done->SetEventOnCompletion(serial,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU completion");
        check(!control.execution_readback(other.Get()),"Other queue cannot claim execution");auto proof=control.execution_readback(queue.Get());check(bool(proof),"Readback witness");std::array<std::array<UINT64,2>,GpuControl::capacity> actual{};check(proof->read(actual),"Completed witness");check((actual[0]==proof->expected[0])==(iteration!=1),"Only actually executed shader may prove epoch, including upper 32 bits");held.push_back(*proof);
    }
    arc::dx12::optimizer::ControlValue active{2,1,16,16};check(control.prepare(queue.Get(),{&active,1})==nullptr,"Pinned capture slots cannot be overwritten");
    std::array<std::array<UINT64,2>,GpuControl::capacity> original_marker{};check(held.front().read(original_marker)&&original_marker[0]==held.front().expected[0],"Captured marker remains immutable across reuse attempts");
    for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T size{};hr(diagnostics->GetMessage(i,nullptr,&size));std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(diagnostics->GetMessage(i,message,&size));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';check(false,"Debug layer error");}}
    CloseHandle(event);std::cout<<"GPU-written execution epoch, predication rejection, queue identity and pinned readback PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
