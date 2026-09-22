#include <windows.h>
#include <d3d12.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "generic_mip_transform.hpp"
#include "generic_ir_hints.hpp"
#include <filesystem>
#include <vector>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <source_location>
using Microsoft::WRL::ComPtr;
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void hr(HRESULT v,std::source_location where=std::source_location::current()){if(FAILED(v))throw std::runtime_error("HRESULT "+std::to_string(v)+" at line "+std::to_string(where.line()));}
ComPtr<IDxcBlob> result(IDxcOperationResult* op){HRESULT status{};hr(op->GetStatus(&status));if(FAILED(status)){ComPtr<IDxcBlobEncoding> e;op->GetErrorBuffer(&e);if(e)std::cerr.write(static_cast<const char*>(e->GetBufferPointer()),e->GetBufferSize());hr(status);}ComPtr<IDxcBlob> b;hr(op->GetResult(&b));return b;}
struct Compiler {
    HMODULE module{},validator_module{};ComPtr<IDxcLibrary> library;ComPtr<IDxcCompiler> compiler;ComPtr<IDxcAssembler> assembler;ComPtr<IDxcValidator> validator;
    explicit Compiler(const std::filesystem::path& path){
        check(path.is_absolute(),"Absolute compiler path required");
        const auto flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
        validator_module=LoadLibraryExW((path.parent_path()/"dxil.dll").c_str(),nullptr,flags);module=LoadLibraryExW(path.c_str(),nullptr,flags);check(module&&validator_module,"Load DXC");
        auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module,"DxcCreateInstance"));auto vc=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator_module,"DxcCreateInstance"));check(create&&vc,"DXC exports");
        hr(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)));hr(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));hr(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler)));hr(vc(CLSID_DxcValidator,IID_PPV_ARGS(&validator)));
    }
    ~Compiler(){validator.Reset();assembler.Reset();compiler.Reset();library.Reset();if(module)FreeLibrary(module);if(validator_module)FreeLibrary(validator_module);}
    ComPtr<IDxcBlobEncoding> blob(const std::string& s){ComPtr<IDxcBlobEncoding> b;hr(library->CreateBlobWithEncodingOnHeapCopy(s.data(),static_cast<UINT32>(s.size()),CP_UTF8,&b));return b;}
    ComPtr<IDxcBlob> compile(const std::string& s,const wchar_t* profile=L"cs_6_0"){auto b=blob(s);ComPtr<IDxcOperationResult> op;const wchar_t* flags[]{L"-enable-16bit-types"};const bool half=std::wstring(profile)==L"cs_6_2";hr(compiler->Compile(b.Get(),L"fixture",L"MainCS",profile,half?flags:nullptr,half?1:0,nullptr,0,nullptr,&op));return result(op.Get());}
    std::string disassemble(IDxcBlob* b){ComPtr<IDxcBlobEncoding> text;hr(compiler->Disassemble(b,&text));return {static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize()};}
    ComPtr<IDxcBlob> assemble(const std::string& s){auto b=blob(arc::dx12::shader::preserve_arc_branches(s));ComPtr<IDxcOperationResult> op;hr(assembler->AssembleToContainer(b.Get(),&op));auto binary=result(op.Get());op.Reset();hr(validator->Validate(binary.Get(),DxcValidatorFlags_InPlaceEdit,&op));return result(op.Get());}
};

int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"Absolute DXC path required");Compiler compiler(argv[1]);
    auto vs=compiler.compile("struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O MainCS(uint i:SV_VertexID){O o;float2 p=float2((i<<1)&2,i&2);o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);o.uv=p;return o;}",L"vs_6_0");
    auto hlsl=[](double bias){return std::string("Texture2D<float4> t:register(t0);SamplerState s:register(s0);float4 MainCS(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.SampleBias(s,uv,")+std::to_string(bias)+")+t.SampleBias(s,uv,"+std::to_string(bias)+"+.5)+t.SampleLevel(s,uv,"+std::to_string(bias)+"+1);}";};
    auto original_source=hlsl(0);auto implicit=original_source.find("t.SampleBias(s,uv,0.000000)");original_source.replace(implicit,std::string("t.SampleBias(s,uv,0.000000)").size(),"t.Sample(s,uv)");
    auto original=compiler.compile(original_source,L"ps_6_0");const auto ir=compiler.disassemble(original.Get());
    auto neutral=arc::dx12::shader::bias_pixel_mips(ir,0);check(neutral.samples==3&&neutral.ir==ir,"Neutral pixel IR exact");
    check(!arc::dx12::shader::bias_pixel_mips(compiler.disassemble(vs.Get()),2).samples,"Non-pixel stage rejected");
    auto unsafe=compiler.compile("Texture2D<float4> t:register(t0);SamplerState s:register(s0);RWTexture2D<float4> u:register(u0);float4 MainCS(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{float4 v=t.Sample(s,uv);u[uint2(p.xy)]=v;return v;}",L"ps_6_0");
    check(!arc::dx12::shader::bias_pixel_mips(compiler.disassemble(unsafe.Get()),2).samples,"Pixel UAV writes rejected");
    check(!arc::dx12::shader::bias_pixel_mips(ir,9).samples,"Out of range mip control rejected");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
    auto begin=[&]{hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));};
    auto execute=[&]{hr(list->Close());ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU fence timeout");};
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);};
    auto resource=[&](D3D12_RESOURCE_DESC d,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state){D3D12_HEAP_PROPERTIES h{};h.Type=heap;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
    auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return resource(d,heap,state);};
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=8;td.DepthOrArraySize=td.SampleDesc.Count=1;td.MipLevels=4;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto texture=resource(td,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[4]{};UINT64 bytes{};device->GetCopyableFootprints(&td,0,4,0,footprints,nullptr,nullptr,&bytes);auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* ptr{};D3D12_RANGE empty{};hr(upload->Map(0,&empty,&ptr));for(unsigned m=0;m<4;++m)for(unsigned y=0;y<(8u>>m);++y)for(unsigned x=0;x<(8u>>m);++x){float color[]{.125f*m,.25f*m,.0625f*m,1};std::memcpy(static_cast<char*>(ptr)+footprints[m].Offset+y*footprints[m].Footprint.RowPitch+x*16,color,16);}upload->Unmap(0,nullptr);
    begin();for(unsigned m=0;m<4;++m){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=texture.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;a.SubresourceIndex=m;b.pResource=upload.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprints[m];list->CopyTextureRegion(&a,0,0,0,&b,nullptr);}barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);execute();
    td.Width=td.Height=64;td.MipLevels=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;auto target=resource(td,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_PLACED_SUBRESOURCE_FOOTPRINT rt_layout{};device->GetCopyableFootprints(&td,0,1,0,&rt_layout,nullptr,nullptr,&bytes);auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=1;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;ComPtr<ID3D12DescriptorHeap> rt_heap,srv_heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt_heap)));device->CreateRenderTargetView(target.Get(),nullptr,rt_heap->GetCPUDescriptorHandleForHeapStart());hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv_heap)));device->CreateShaderResourceView(texture.Get(),nullptr,srv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={1,&range};param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};ComPtr<ID3DBlob> root_blob;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    auto render=[&](IDxcBlob* pixel){D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=td.Format;p.SampleDesc.Count=1;ComPtr<ID3D12PipelineState> pipeline;hr(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));
        begin();list->SetPipelineState(pipeline.Get());list->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootDescriptorTable(0,srv_heap->GetGPUDescriptorHandleForHeapStart());const auto rtv=rt_heap->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);float clear[4]{};list->ClearRenderTargetView(rtv,clear,0,nullptr);D3D12_VIEWPORT vp{0,0,64,64,0,1};D3D12_RECT rect{0,0,64,64};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&rect);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);barrier(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=readback.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=rt_layout;b.pResource=target.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&a,0,0,0,&b,nullptr);barrier(target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);execute();D3D12_RANGE read{0,SIZE_T(bytes)};hr(readback->Map(0,&read,&ptr));std::vector<char> pixels(64*64*16);for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64*16,static_cast<char*>(ptr)+y*rt_layout.Footprint.RowPitch,64*16);readback->Unmap(0,&empty);return pixels;};
    const auto reference=render(original.Get());auto unchanged=compiler.assemble(neutral.ir);check(render(unchanged.Get())==reference,"Neutral pixels exact");
    for(unsigned half_steps:{1u,2u,3u,4u,8u}){const double bias=half_steps*.5;auto transformed=arc::dx12::shader::bias_pixel_mips(ir,half_steps);check(transformed.samples==3,"Three sample forms transformed");auto binary=compiler.assemble(transformed.ir);auto oracle=compiler.compile(hlsl(bias),L"ps_6_0");auto actual=render(binary.Get());check(actual==render(oracle.Get()),"Every pixel matches independent HLSL mip oracle");check(actual!=reference,"Transformation must have an effect");check(render(original.Get())==reference,"Original restored exactly");}
    CloseHandle(event);std::cout<<"Pixel Sample/SampleBias/SampleLevel GPU oracle, neutral and rollback PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
