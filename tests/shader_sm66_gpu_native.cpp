#include "generic_shader_transform.hpp"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void hr(HRESULT value){require(SUCCEEDED(value),"D3D12/DXC HRESULT");}
ComPtr<IDxcBlob> result(IDxcOperationResult* op){HRESULT status{};hr(op->GetStatus(&status));
    if(FAILED(status)){ComPtr<IDxcBlobEncoding> errors;op->GetErrorBuffer(&errors);
        if(errors)std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());hr(status);}
    ComPtr<IDxcBlob> blob;hr(op->GetResult(&blob));return blob;}
struct Dxc {
    HMODULE module{},validator_module{};ComPtr<IDxcLibrary> library;ComPtr<IDxcCompiler> compiler;
    ComPtr<IDxcAssembler> assembler;ComPtr<IDxcValidator> validator;
    explicit Dxc(const std::filesystem::path& path){require(path.is_absolute(),"Absolute DXC path required");
        constexpr auto flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
        validator_module=LoadLibraryExW((path.parent_path()/"dxil.dll").c_str(),nullptr,flags);
        module=LoadLibraryExW(path.c_str(),nullptr,flags);require(module&&validator_module,"Load DXC");
        const auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module,"DxcCreateInstance"));
        const auto vc=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator_module,"DxcCreateInstance"));
        require(create&&vc,"DXC factories");hr(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)));
        hr(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));hr(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler)));
        hr(vc(CLSID_DxcValidator,IID_PPV_ARGS(&validator)));
    }
    ~Dxc(){validator.Reset();assembler.Reset();compiler.Reset();library.Reset();if(module)FreeLibrary(module);if(validator_module)FreeLibrary(validator_module);}
    ComPtr<IDxcBlobEncoding> blob(const std::string& source){ComPtr<IDxcBlobEncoding> out;
        hr(library->CreateBlobWithEncodingOnHeapCopy(source.data(),static_cast<UINT32>(source.size()),CP_UTF8,&out));return out;}
    std::pair<ComPtr<IDxcBlob>,std::string> compile(){const auto source=blob(R"(
        RWTexture2D<float4> output:register(u0);
        [numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=float4(p.xy,0,1);})");
        ComPtr<IDxcOperationResult> op;hr(compiler->Compile(source.Get(),L"sm66-gpu",L"MainCS",L"cs_6_6",nullptr,0,nullptr,0,nullptr,&op));
        auto binary=result(op.Get());ComPtr<IDxcBlobEncoding> disassembly;hr(compiler->Disassemble(binary.Get(),&disassembly));
        return {binary,{static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize()}};}
    ComPtr<IDxcBlob> assemble(const std::string& ir){auto text=blob(ir);ComPtr<IDxcOperationResult> op;
        hr(assembler->AssembleToContainer(text.Get(),&op));auto binary=result(op.Get());op.Reset();
        hr(validator->Validate(binary.Get(),DxcValidatorFlags_InPlaceEdit,&op));return result(op.Get());}
};
struct Pixel {float x,y,z,w;};
}
int wmain(int argc,wchar_t** argv)try{
    require(argc==2,"Usage: shader-sm66-gpu-native ABSOLUTE_DXCOMPILER_DLL");Dxc dxc(argv[1]);
    auto [original,ir]=dxc.compile();
    auto transform=[&](unsigned x,unsigned y,bool controlled){auto changed=arc::dx12::shader::coarse_compute(ir,x,y,controlled);
        require(changed.admitted,changed.reason.c_str());return std::pair{dxc.assemble(changed.ir),changed.control_space};};
    auto [neutral,unused0]=transform(1,1,false);
    auto [coarse,unused1]=transform(2,2,false);
    auto [controlled,control_space]=transform(1,1,true);
    require(control_space!=UINT32_MAX,"Controlled binding");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable={1,&range};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor={0,control_space};
    D3D12_ROOT_SIGNATURE_DESC root_desc{};root_desc.NumParameters=2;root_desc.pParameters=params;
    ComPtr<ID3DBlob> serialized,error;hr(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error));
    ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    IDxcBlob* variants[]{original.Get(),neutral.Get(),coarse.Get(),controlled.Get()};
    for(unsigned i=0;i<4;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
        desc.CS={variants[i]->GetBufferPointer(),variants[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipelines[i])));}
    constexpr UINT width=16,height=16;
    D3D12_RESOURCE_DESC image_desc{};image_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image_desc.Width=width;image_desc.Height=height;image_desc.DepthOrArraySize=1;image_desc.MipLevels=1;
    image_desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;image_desc.SampleDesc.Count=1;
    image_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> output;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&image_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
    device->GetCopyableFootprints(&image_desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer_desc{};buffer_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width=bytes;buffer_desc.Height=buffer_desc.DepthOrArraySize=buffer_desc.MipLevels=buffer_desc.SampleDesc.Count=1;
    buffer_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    buffer_desc.Width=256;heap.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> control;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&control)));
    D3D12_DESCRIPTOR_HEAP_DESC view_desc{};view_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;view_desc.NumDescriptors=1;
    view_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> views;
    hr(device->CreateDescriptorHeap(&view_desc,IID_PPV_ARGS(&views)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=image_desc.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,views->GetCPUDescriptorHandleForHeapStart());
    D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;
    hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));ComPtr<ID3D12CommandAllocator> allocator;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Create fence event");UINT64 serial{};
    auto barrier=[&](D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER transition{};
        transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition.pResource=output.Get();
        transition.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        transition.Transition.StateBefore=from;transition.Transition.StateAfter=to;list->ResourceBarrier(1,&transition);};
    auto execute=[&](unsigned pipeline,unsigned rate,unsigned expected_rate){
        void* mapped{};D3D12_RANGE empty{};hr(control->Map(0,&empty,&mapped));
        auto* words=static_cast<UINT*>(mapped);words[0]=words[1]=rate;words[2]=width;words[3]=height;
        D3D12_RANGE written{0,16};control->Unmap(0,&written);
        hr(allocator->Reset());hr(list->Reset(allocator.Get(),pipelines[pipeline].Get()));
        list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootConstantBufferView(1,control->GetGPUVirtualAddress());list->Dispatch(2,2,1);
        barrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);hr(list->Close());
        ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
        hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));
        require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU fence timeout");
        D3D12_RANGE read{0,static_cast<SIZE_T>(bytes)};hr(readback->Map(0,&read,&mapped));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto* pixel=reinterpret_cast<const Pixel*>(
            static_cast<const char*>(mapped)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel));
            const float expected_x=float((x/expected_rate)*expected_rate),expected_y=float((y/expected_rate)*expected_rate);
            if(pixel->x!=expected_x||pixel->y!=expected_y||pixel->z!=0.f||pixel->w!=1.f){
                readback->Unmap(0,nullptr);throw std::runtime_error("SM6.6 GPU output mismatch at "+std::to_string(x)+","+std::to_string(y));}}
        D3D12_RANGE none{};readback->Unmap(0,&none);
    };
    execute(0,1,1);execute(1,1,1);execute(2,1,2);execute(3,1,1);execute(3,2,2);execute(0,1,1);
    CloseHandle(event);std::cout<<"SM6.6 GPU neutral, coarsening, control, and rollback passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
