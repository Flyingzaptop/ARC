#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "generic_shader_transform.hpp"
#include "generic_gpu_control.hpp"
#include "generic_uniform_access.hpp"
#include "generic_pcf_transform.hpp"
#include "generic_zero_transform.hpp"
#include "generic_ir_hints.hpp"
#include "generic_edge_transform.hpp"
#include "generic_mip_transform.hpp"
#include <algorithm>
#include <regex>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
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
    ComPtr<IDxcBlob> compile(const std::string& s,const wchar_t* profile=L"cs_6_0"){auto b=blob(s);ComPtr<IDxcOperationResult> op;hr(compiler->Compile(b.Get(),L"fixture",L"MainCS",profile,nullptr,0,nullptr,0,nullptr,&op));return result(op.Get());}
    std::string disassemble(IDxcBlob* b){ComPtr<IDxcBlobEncoding> text;hr(compiler->Disassemble(b,&text));return {static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize()};}
    ComPtr<IDxcBlob> assemble(const std::string& s){auto b=blob(arc::dx12::shader::preserve_arc_branches(s));ComPtr<IDxcOperationResult> op;hr(assembler->AssembleToContainer(b.Get(),&op));auto binary=result(op.Get());op.Reset();hr(validator->Validate(binary.Get(),DxcValidatorFlags_InPlaceEdit,&op));return result(op.Get());}
};
struct Pixel {float r,g,b,a;};
int wmain(int argc,wchar_t** argv)try{
    check(argc==3||argc==4,"Compiler DLL, fresh result directory and optional probe DLL required");const std::filesystem::path directory=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(directory),"Fresh directory required");std::filesystem::create_directories(directory);
    Compiler compiler(argv[1]);
    const std::string source=R"(
Texture2D<float4> input_color[5]:register(t3,space2);
RWTexture2D<float4> output_color:register(u4,space3);
RWTexture2D<float4> output_aux:register(u7,space3);
[numthreads(8,8,1)] void MainCS(uint3 pixel:SV_DispatchThreadID) {
    // The original 37-high dispatch has five groups. A coarse transform must
    // not execute new padded rows that would index outside this array.
    float4 c=input_color[pixel.y/8][pixel.xy];
    // Uniform in the original 8x8 group, but x coarsening crosses descriptor
    // groups inside one hardware wave. Distinct inputs expose stale hints.
    c.r+=input_color[(pixel.x/8)%5][pixel.xy].r;
    if(c.a==0){output_aux[pixel.xy]=0;return;}
    output_color[pixel.xy]=float4(c.r*2,c.g+0.125,c.b*0.5,1);
    output_aux[pixel.xy]=float4(c.b,c.r,c.g,0.5);
})";
    auto original=compiler.compile(source);auto ir=compiler.disassemble(original.Get());
    {ComPtr<ID3DBlob> legacy,errors;hr(D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,"MainCS","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&legacy,&errors));std::ofstream file(directory/"legacy-input.bin",std::ios::binary);file.write(static_cast<const char*>(legacy->GetBufferPointer()),legacy->GetBufferSize());check(bool(file),"Write independent DXBC fixture");}
    const auto neutral=arc::dx12::shader::coarse_compute(ir,1,1),coarse=arc::dx12::shader::coarse_compute(ir,2,2);
    check(neutral.admitted&&coarse.admitted,"Independent typed pixel writes should be admitted");
    check(coarse.resources.size()==3&&coarse.stores==3,"All bindings/stores captured");
    auto neutral_code=compiler.assemble(neutral.ir),coarse_code=compiler.assemble(coarse.ir);
    const auto controlled=arc::dx12::shader::coarse_compute(ir,1,1,true);
    check(controlled.admitted&&controlled.control_space!=UINT32_MAX,"Dynamic control binding");
    auto controlled_code=compiler.assemble(controlled.ir);
    unsigned rejected=0;
    for(const auto& bad:std::array<std::string,3>{
        "RWTexture2D<float4> image:register(u0); [numthreads(8,8,1)] void MainCS(uint3 id:SV_DispatchThreadID){image[id.xy]=image[id.xy]+1;}",
        "RWTexture2D<uint> image:register(u0); [numthreads(8,8,1)] void MainCS(uint3 id:SV_DispatchThreadID){InterlockedAdd(image[id.xy],1);}",
        "RWTexture2D<float4> image:register(u0); [numthreads(8,8,1)] void MainCS(uint3 id:SV_DispatchThreadID){image[id.yx]=float4(id.xy,0,1);}"
    }){auto b=compiler.compile(bad);check(!arc::dx12::shader::coarse_compute(compiler.disassemble(b.Get()),2,2).admitted,"Unsafe shader must be rejected");++rejected;}
    check(!arc::dx12::shader::coarse_compute("malformed",2,2).admitted,"Malformed IR rejected");
    check(!arc::dx12::shader::coarse_compute(ir,4,2).admitted,"Unsupported rate rejected");
    const auto loop=compiler.compile(R"(
cbuffer Settings:register(b9,space4){int count;int3 padding;int4 entries[16];}
Texture2D<float4> images[15]:register(t7,space2);
RWTexture2D<float4> target:register(u5,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){float4 c=0;for(int i=0;i<count;i++){int j=entries[i].x;if(j>=0)c+=images[j][p.xy];}target[p.xy]=c;}
)");
    const auto access=arc::dx12::shader::UniformAccessProgram::compile(compiler.disassemble(loop.Get()));check(access!=nullptr,"Uniform access program compilation");
    auto reader=[](unsigned range,unsigned reg,unsigned offset){check(range==0&&reg==9,"CBV identity must come from bytecode");arc::dx12::shader::UniformWords words;words.valid_mask=15;if(offset==0)words.words[0]=3;else if(offset==16)words.words[0]=0;else if(offset==32)words.words[0]=4;else words.words[0]=UINT32_MAX;return words;};
    const auto usage=access->evaluate(reader);check(usage.complete,"Uniform resource proof must terminate");
    const auto used=usage.ranges.at({0,0});check(!used.all&&used.indices==std::set<unsigned>{7,11},"Only proven reachable descriptor indices may be selected");
    check(!access->evaluate(reader,1).complete,"Access budget exhaustion must decline");
    check(!access->evaluate([](unsigned,unsigned,unsigned){return arc::dx12::shader::UniformWords{};},512).complete,"Unreadable dynamic loop bound must decline");

    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    using Api=DWORD(WINAPI*)(void*);Api arc_mode{},arc_snapshot{};auto arc_path=(directory/L"arc.json").wstring();
    if(argc==4){HMODULE probe=LoadLibraryW(argv[3]);check(probe!=nullptr,"Load generic optimizer DLL");auto init=reinterpret_cast<Api>(GetProcAddress(probe,"ArcInitialize"));auto lean=reinterpret_cast<Api>(GetProcAddress(probe,"ArcUseLeanMode"));arc_mode=reinterpret_cast<Api>(GetProcAddress(probe,"ArcExperimentalCompute"));arc_snapshot=reinterpret_cast<Api>(GetProcAddress(probe,"ArcSnapshot"));check(init&&lean&&arc_mode&&arc_snapshot&&init(arc_path.data())==0&&lean(nullptr)==0,"Initialize generic optimizer");}
    auto wait_prepared=[&](unsigned count){if(!arc_snapshot)return;const auto deadline=GetTickCount64()+10000;bool ready=false;while(GetTickCount64()<deadline){check(arc_snapshot(nullptr)==0,"Optimizer snapshot");std::ifstream file(arc_path);std::string json{std::istreambuf_iterator<char>(file),{}};std::smatch match;if(std::regex_search(json,match,std::regex("\"prepared\":([0-9]+)"))&&std::stoul(match[1])>=count){ready=true;break;}Sleep(20);}check(ready,"Generic worker must prepare a real shader variant");};
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;hr(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));ComPtr<ID3D12InfoQueue> diagnostics;hr(device.As(&diagnostics));
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    arc::dx12::optimizer::GpuControl policy(device.Get());arc::dx12::optimizer::ControlValue desired;
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Fence event");
    auto begin=[&]{hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));};
    auto submit=[&]{auto* helper=policy.prepare(queue.Get(),{&desired,1});if(helper)queue->ExecuteCommandLists(1,&helper);ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);policy.submitted(queue.Get());hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU timeout");};
    auto execute=[&]{policy.record_neutralize(list.Get());hr(list->Close());submit();};
    auto proven=[&]{check(arc_snapshot&&arc_snapshot(nullptr)==0,"Uniform proof snapshot");std::ifstream file(arc_path);std::string text{std::istreambuf_iterator<char>(file),{}};std::uint64_t sum=0;const std::regex field("\"uniform_proven\":([0-9]+)");for(auto it=std::sregex_iterator(text.begin(),text.end(),field);it!=std::sregex_iterator();++it)sum+=std::stoull((*it)[1]);return sum;};
    auto await_proof=[&](std::uint64_t previous,const wchar_t* restore){check(arc_mode(const_cast<wchar_t*>(L"neutral"))==0,"Neutral during asynchronous proof warmup");const auto deadline=GetTickCount64()+5000;
        while(proven()<=previous&&GetTickCount64()<deadline){Sleep(10);submit();}check(proven()>previous,"Asynchronous uniform proof must finish on bounded native input");check(arc_mode(const_cast<wchar_t*>(restore))==0,"Restore requested experimental policy");submit();};
    D3D12_DESCRIPTOR_RANGE ranges[3]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,3,2,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,4,3,5},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,7,3,6}};
    D3D12_ROOT_PARAMETER parameters[6]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={3,ranges};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[1].Descriptor={0,controlled.control_space};
    D3D12_DESCRIPTOR_RANGE unused_bindless{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,42,0};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[2].DescriptorTable={1,&unused_bindless};
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={9,7,2};
    D3D12_DESCRIPTOR_RANGE unused_outputs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,UINT_MAX,0,43,0};
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[4].DescriptorTable={1,&unused_outputs};
    parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[5].Descriptor={8,9};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=6;rd.pParameters=parameters;
    ComPtr<ID3DBlob> root_blob,error;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error));ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    IDxcBlob* codes[]{original.Get(),neutral_code.Get(),coarse_code.Get(),controlled_code.Get()};
    for(unsigned i=0;i<4;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={codes[i]->GetBufferPointer(),codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipelines[i])));}
    wait_prepared(1);
    ComPtr<ID3D12RootSignature> duplicate_root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&duplicate_root)));
    const bool interned_root=duplicate_root.Get()==root.Get();duplicate_root.Reset();
    constexpr UINT width=61,height=37;
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=height;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    std::array<ComPtr<ID3D12Resource>,3> textures;
    hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[0])));
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for(unsigned i=1;i<3;++i)hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[i])));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload,initial;hp.Type=D3D12_HEAP_TYPE_UPLOAD;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&initial)));
    std::array<ComPtr<ID3D12Resource>,2> readbacks;hp.Type=D3D12_HEAP_TYPE_READBACK;for(auto& b:readbacks)hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&b)));
    const Pixel clear{.75f,.75f,.75f,.75f};std::vector<Pixel> input(width*height);
    void* ptr{};D3D12_RANGE empty{};hr(upload->Map(0,&empty,&ptr));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){auto& p=input[y*width+x];p={x/64.f,y/64.f,(x+y)/128.f,x%7?1.f:0.f};std::memcpy(static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),&p,sizeof(Pixel));}upload->Unmap(0,nullptr);
    hr(initial->Map(0,&empty,&ptr));for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)std::memcpy(static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),&clear,sizeof(Pixel));initial->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=7;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto handle=heap->GetCPUDescriptorHandleForHeapStart();const auto increment=device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    for(unsigned i=0;i<5;++i){device->CreateShaderResourceView(textures[0].Get(),&srv,handle);if(i<4)handle.ptr+=increment;}
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=td.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    for(unsigned i=1;i<3;++i){handle.ptr+=increment;device->CreateUnorderedAccessView(textures[i].Get(),nullptr,&uav,handle);}
    auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,0,before,after};list->ResourceBarrier(1,&b);};
    auto copy_in=[&](ID3D12Resource* dst,ID3D12Resource* src){D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=dst;d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;s.pResource=src;s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=footprint;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);};
    begin();copy_in(textures[0].Get(),upload.Get());transition(textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);execute();
    auto array_pixel=[&](UINT x,UINT y,UINT index){auto p=input[y*width+x];const auto bias=float(index)/16;p.r+=bias;p.g+=bias;p.b+=bias;return p;};
    std::array<ComPtr<ID3D12Resource>,4> distinct_sources;
    for(unsigned i=0;i<distinct_sources.size();++i){auto d=td;d.Flags=D3D12_RESOURCE_FLAG_NONE;hp.Type=D3D12_HEAP_TYPE_DEFAULT;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&distinct_sources[i])));
        hr(upload->Map(0,&empty,&ptr));for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto p=array_pixel(x,y,i+1);std::memcpy(static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),&p,sizeof(Pixel));}upload->Unmap(0,nullptr);
        begin();copy_in(distinct_sources[i].Get(),upload.Get());transition(distinct_sources[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);execute();
        auto destination=heap->GetCPUDescriptorHandleForHeapStart();destination.ptr+=(i+1)*increment;device->CreateShaderResourceView(distinct_sources[i].Get(),&srv,destination);
    }
    std::array<std::vector<Pixel>,2> baseline;
    const unsigned x_rates[]{1,1,2,1,2,1,2,1,UINT32_MAX,1,2,1,2,1},y_rates[]{1,1,2,1,1,2,2,1,0,1,1,2,2,1};
    for(unsigned mode=0;mode<(arc_mode?14u:9u);++mode){
        desired={x_rates[mode],y_rates[mode],width,height};
        if(mode>=9){const wchar_t* modes[]{L"neutral",L"2x1",L"1x2",L"2x2",L"off"};check(arc_mode(const_cast<wchar_t*>(modes[mode-9]))==0,"Generic policy switch");}
        if(mode<4||mode==9){
        begin();for(unsigned i=1;i<3;++i){copy_in(textures[i].Get(),initial.Get());transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(pipelines[mode==9?0:mode].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
        for(unsigned i=1;i<3;++i){transition(textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[i-1].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[i].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);}execute();
        }else submit(); // Replay exactly the same closed native recording.
        for(unsigned target=0;target<2;++target){D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[target]->Map(0,&range,&ptr));std::vector<Pixel> pixels(width*height);for(UINT y=0;y<height;++y)std::memcpy(pixels.data()+y*width,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch,width*sizeof(Pixel));readbacks[target]->Unmap(0,&empty);
            if(mode==0)baseline[target]=pixels;
            if(mode==1||mode==3||mode==7||mode==8||mode==9||mode==13)check(std::memcmp(pixels.data(),baseline[target].data(),pixels.size()*sizeof(Pixel))==0,"Neutral transform and cached rollback must be bit-exact");
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT sx=x_rates[mode]==2?x&~1u:x,sy=y_rates[mode]==2?y&~1u:y;auto p=array_pixel(sx,sy,sy/8);p.r+=array_pixel(sx,sy,(sx/8)%5).r;const Pixel expected=p.a?(target?Pixel{p.b,p.r,p.g,.5f}:Pixel{p.r*2,p.g+.125f,p.b*.5f,1}):(target?Pixel{}:clear);check(std::memcmp(&pixels[y*width+x],&expected,sizeof(Pixel))==0,"Every output pixel including edges and conditional stores must match");}
        }
    }
    // Independent comparison-filter fixture: no engine labels, 25 point depth
    // comparisons with known CPU results, then dynamic 9-tap selection/rollback.
    const std::string pcf_source=R"(
Texture2D<float> depth_image:register(t3,space2);
SamplerComparisonState comparison:register(s2,space2);
RWTexture2D<float4> target:register(u4,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){
 float2 uv=(float2(p.xy)+0.5)/float2(61,37);float sum=0;
 [unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++)sum+=depth_image.SampleCmpLevelZero(comparison,uv,0.5,int2(x,y));
 float filtered=sum/25.0;target[p.xy]=float4(filtered,filtered,filtered,1);
})";
    auto pcf_original=compiler.compile(pcf_source);const auto pcf_base=arc::dx12::shader::coarse_compute(compiler.disassemble(pcf_original.Get()),1,1,true);
    check(pcf_base.admitted,"Comparison shader classification");const auto pcf=arc::dx12::shader::sparse_comparison_filter(pcf_base.ir);
    check(pcf.groups==1&&pcf.samples_removed==16,"Recognize exactly one normalized 25-tap filter");auto pcf_code=compiler.assemble(pcf.ir);
    auto wrong_weight=pcf_source;wrong_weight.replace(wrong_weight.find("/25.0"),5,"/24.0");auto bad_weight=compiler.compile(wrong_weight);
    const auto bad_base=arc::dx12::shader::coarse_compute(compiler.disassemble(bad_weight.Get()),1,1,true);
    check(bad_base.admitted&&arc::dx12::shader::sparse_comparison_filter(bad_base.ir).groups==0,"Do not rewrite unrelated/non-normalized sums");
    D3D12_STATIC_SAMPLER_DESC comparison{};comparison.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;comparison.AddressU=comparison.AddressV=comparison.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    comparison.ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;comparison.MaxLOD=D3D12_FLOAT32_MAX;comparison.ShaderRegister=2;comparison.RegisterSpace=2;
    rd.NumStaticSamplers=1;rd.pStaticSamplers=&comparison;root_blob.Reset();error.Reset();hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error));
    ComPtr<ID3D12RootSignature> pcf_root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&pcf_root)));
    std::array<ComPtr<ID3D12PipelineState>,2> pcf_pipelines;IDxcBlob* pcf_codes[]{pcf_original.Get(),pcf_code.Get()};
    for(unsigned i=0;i<2;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=pcf_root.Get();p.CS={pcf_codes[i]->GetBufferPointer(),pcf_codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pcf_pipelines[i])));}
    wait_prepared(2);
    auto depth_desc=td;depth_desc.Flags=D3D12_RESOURCE_FLAG_NONE;depth_desc.Format=DXGI_FORMAT_R32_FLOAT;hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> depth_texture,depth_upload;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&depth_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&depth_texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{};UINT64 depth_bytes{};device->GetCopyableFootprints(&depth_desc,0,1,0,&depth_footprint,nullptr,nullptr,&depth_bytes);
    bd.Width=depth_bytes;hp.Type=D3D12_HEAP_TYPE_UPLOAD;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&depth_upload)));
    std::vector<float> depth_values(width*height);hr(depth_upload->Map(0,&empty,&ptr));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const float d=(x/3+y/5)%2?.8f:.2f;depth_values[y*width+x]=d;std::memcpy(static_cast<char*>(ptr)+y*depth_footprint.Footprint.RowPitch+x*4,&d,4);}depth_upload->Unmap(0,nullptr);
    desired={1,1,width,height,0};begin();D3D12_TEXTURE_COPY_LOCATION depth_dst{},depth_src{};depth_dst.pResource=depth_texture.Get();depth_dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;depth_src.pResource=depth_upload.Get();depth_src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;depth_src.PlacedFootprint=depth_footprint;list->CopyTextureRegion(&depth_dst,0,0,0,&depth_src,nullptr);transition(depth_texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);execute();
    srv.Format=DXGI_FORMAT_R32_FLOAT;device->CreateShaderResourceView(depth_texture.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
    std::vector<Pixel> pcf_reference;
    for(unsigned mode=0;mode<(arc_mode?7u:4u);++mode){desired={1,1,width,height,mode==2?9u:0u};const bool reduced=mode==2||mode==5;
        // Neutral records a reversible variant for cached replay. Off deliberately
        // leaves newly recorded application work uninstrumented.
        if(mode>=4){const wchar_t* value=mode==4?L"neutral":mode==5?L"pcf9":L"off";check(arc_mode(const_cast<wchar_t*>(value))==0,"Injected comparison filter switch");}
        if(mode<2||mode==4){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(pcf_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(pcf_pipelines[mode==4?0:mode].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
            transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        }else submit();
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));std::vector<Pixel> actual(width*height);for(UINT y=0;y<height;++y)std::memcpy(actual.data()+y*width,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch,width*sizeof(Pixel));readbacks[0]->Unmap(0,&empty);
        if(!mode)pcf_reference=actual;else if(!reduced)check(std::memcmp(actual.data(),pcf_reference.data(),actual.size()*sizeof(Pixel))==0,"Comparison filter neutral/rollback must be bit-exact");
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){unsigned samples=0;for(int dy=-2;dy<=2;dy+=reduced?2:1)for(int dx=-2;dx<=2;dx+=reduced?2:1){const int sx=std::clamp(int(x)+dx,0,int(width)-1),sy=std::clamp(int(y)+dy,0,int(height)-1);samples+=depth_values[sy*width+sx]>=.5f;}
            const float expected=float(samples)*(reduced?1.f/9.f:1.f/25.f);check(std::abs(actual[y*width+x].r-expected)<1.e-7f&&actual[y*width+x].a==1,"Comparison filtering must match the independent CPU oracle");}
    }
    const std::string zero_source=R"(
Texture2D<float4> source:register(t3,space2);RWTexture2D<float4> target:register(u4,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){float4 c=source[p.xy];float value=0;
 [branch]if(c.a>0){float gate=saturate(c.r-0.4);float3 v=c.rgb+0.01;
 [unroll]for(int i=0;i<24;i++){v=sin(v*1.1+i*0.031)*0.5+0.5;}
 value=dot(v,float3(0.2,0.3,0.4))*gate;}
 target[p.xy]=float4(value,value*0.5,value*0.25,1);
})";
    auto zero_original=compiler.compile(zero_source);const auto zero_base=arc::dx12::shader::coarse_compute(compiler.disassemble(zero_original.Get()),1,1,true);
    check(zero_base.admitted,"Zero-factor shader class");const auto zero=arc::dx12::shader::short_circuit_zero_factors(zero_base.ir);
    check(zero.regions>0,"Discover a pure region with a zero multiplier");auto zero_code=compiler.assemble(zero.ir);
    auto side_effect_source=zero_source;side_effect_source.insert(0,"RWTexture2D<float4> audit:register(u7,space3);\n");
    const auto contribution=side_effect_source.find("value=dot");side_effect_source.insert(contribution,"audit[p.xy]=float4(v,1); ");
    auto side_effect=compiler.compile(side_effect_source);auto side_base=arc::dx12::shader::coarse_compute(compiler.disassemble(side_effect.Get()),1,1,true);
    check(side_base.admitted&&arc::dx12::shader::short_circuit_zero_factors(side_base.ir).regions==0,"A zero product must not suppress another UAV side effect");
    std::array<ComPtr<ID3D12PipelineState>,2> zero_pipelines;IDxcBlob* zero_codes[]{zero_original.Get(),zero_code.Get()};
    for(unsigned i=0;i<2;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=pcf_root.Get();p.CS={zero_codes[i]->GetBufferPointer(),zero_codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&zero_pipelines[i])));}
    wait_prepared(3);
    srv.Format=td.Format;device->CreateShaderResourceView(textures[0].Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());std::vector<Pixel> zero_reference;
    for(unsigned mode=0;mode<4;++mode){desired={1,1,width,height,0,mode==2?1u:0u};
        if(mode<2){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(pcf_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(zero_pipelines[mode].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
            transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        }else submit();
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));std::vector<Pixel> actual(width*height);for(UINT y=0;y<height;++y)std::memcpy(actual.data()+y*width,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch,width*sizeof(Pixel));readbacks[0]->Unmap(0,&empty);
        if(!mode)zero_reference=actual;else check(std::memcmp(actual.data(),zero_reference.data(),actual.size()*sizeof(Pixel))==0,"Zero-factor region must preserve every output bit in enabled/disabled/replayed modes");
    }
    const auto edges=arc::dx12::shader::protect_input_edges(zero_base.ir,zero_base);check(edges.input_mask==1,"Identify screen-aligned float input without names");auto edge_code=compiler.assemble(edges.ir);
    ComPtr<ID3D12PipelineState> edge_pipeline;D3D12_COMPUTE_PIPELINE_STATE_DESC edge_desc{};edge_desc.pRootSignature=pcf_root.Get();edge_desc.CS={edge_code->GetBufferPointer(),edge_code->GetBufferSize()};hr(device->CreateComputePipelineState(&edge_desc,IID_PPV_ARGS(&edge_pipeline)));
    for(unsigned mode=0;mode<3;++mode){desired={2,2,width,height,0,0,0x80000001u,mode==1?3.f:0.f};
        if(!mode){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(pcf_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(edge_pipeline.Get());list->Dispatch((width+7)/8,(height+7)/8,1);
            transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        }else submit();
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));std::vector<Pixel> actual(width*height);for(UINT y=0;y<height;++y)std::memcpy(actual.data()+y*width,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch,width*sizeof(Pixel));readbacks[0]->Unmap(0,&empty);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto& expected=zero_reference[(mode==1?(y&~1u):y)*width+(mode==1?(x&~1u):x)];check(std::memcmp(&actual[y*width+x],&expected,sizeof(Pixel))==0,"Edge protection/coarse grouping must match every pixel including borders");}
    }
    const auto mip_original=compiler.compile(R"(
Texture2D<float4> source:register(t3,space2);SamplerState filtering:register(s2,space2);
RWTexture2D<float4> target:register(u4,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){target[p.xy]=source.SampleLevel(filtering,(float2(p.xy)+.5)/float2(61,37),0);}
)");
    const auto mip_base=arc::dx12::shader::coarse_compute(compiler.disassemble(mip_original.Get()),1,1,true);
    {std::ofstream file(directory/"mip-fixture.ll");file<<mip_base.ir;}
    check(mip_base.admitted,"Explicit mip shader classification");const auto mip=arc::dx12::shader::bias_explicit_mips(mip_base.ir);
    check(mip.samples==1,"Exactly one explicit noncomparison sample");
    check(arc::dx12::shader::bias_explicit_mips(pcf_base.ir).samples==0,"Comparison sampling cannot inherit mip bias");
    check(arc::dx12::shader::bias_explicit_mips(zero_base.ir).samples==0,"Integer texture loads cannot inherit mip bias");
    auto mip_code=compiler.assemble(mip.ir);
    auto regular=comparison;regular.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;regular.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    rd.pStaticSamplers=&regular;root_blob.Reset();error.Reset();hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error));
    ComPtr<ID3D12RootSignature> mip_root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&mip_root)));
    std::array<ComPtr<ID3D12PipelineState>,2> mip_pipelines;IDxcBlob* mip_codes[]{mip_original.Get(),mip_code.Get()};
    for(unsigned i=0;i<2;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=mip_root.Get();p.CS={mip_codes[i]->GetBufferPointer(),mip_codes[i]->GetBufferSize()};
        if(!i){struct alignas(void*) RootItem{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;ID3D12RootSignature* value;};struct alignas(void*) CodeItem{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_SHADER_BYTECODE value;};
            struct {RootItem root;CodeItem code;} stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,p.pRootSignature},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,p.CS}};
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};ComPtr<ID3D12Device2> device2;hr(device.As(&device2));hr(device2->CreatePipelineState(&desc,IID_PPV_ARGS(&mip_pipelines[i])));
        }else hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&mip_pipelines[i])));
    }
    wait_prepared(4);
    auto mip_desc=td;mip_desc.Flags=D3D12_RESOURCE_FLAG_NONE;mip_desc.MipLevels=3;hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> mip_texture,mip_upload;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mip_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&mip_texture)));
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,3> mip_footprints;UINT64 mip_bytes{};device->GetCopyableFootprints(&mip_desc,0,3,0,mip_footprints.data(),nullptr,nullptr,&mip_bytes);
    bd.Width=mip_bytes;hp.Type=D3D12_HEAP_TYPE_UPLOAD;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&mip_upload)));
    const float mip_colors[]{.125f,.375f,.875f};hr(mip_upload->Map(0,&empty,&ptr));
    for(unsigned level=0;level<3;++level){const auto& fp=mip_footprints[level];const Pixel color{mip_colors[level],.25f,.5f,1};
        for(UINT y=0;y<fp.Footprint.Height;++y)for(UINT x=0;x<fp.Footprint.Width;++x)std::memcpy(static_cast<char*>(ptr)+fp.Offset+y*fp.Footprint.RowPitch+x*sizeof(Pixel),&color,sizeof(color));}
    mip_upload->Unmap(0,nullptr);desired={1,1,width,height};begin();
    for(unsigned level=0;level<3;++level){D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=mip_texture.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;d.SubresourceIndex=level;s.pResource=mip_upload.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=mip_footprints[level];list->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={mip_texture.Get(),level,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};list->ResourceBarrier(1,&b);}
    execute();srv.Texture2D.MipLevels=3;device->CreateShaderResourceView(mip_texture.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
    for(unsigned mode=0;mode<(arc_mode?12u:7u);++mode){
        const unsigned steps=mode==2||mode==8?1:mode==3||mode==9?2:mode==4||mode==10?4:mode==5?UINT32_MAX:0;
        desired={1,1,width,height};if(mode<7)desired.mip_steps=steps;
        if(mode>=7){const wchar_t* modes[]{L"neutral",L"mip-half",L"mip1",L"mip2",L"off"};check(arc_mode(const_cast<wchar_t*>(modes[mode-7]))==0,"Injected mip switch");}
        if(mode<2||mode==7){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(mip_pipelines[mode==7?0:mode].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
            transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        }else submit();
        const Pixel expected{steps==1?.25f:steps==2?.375f:steps==4?.875f:.125f,.25f,.5f,1};
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Mip blend, neutral, invalid control and cached rollback must match CPU oracle");}readbacks[0]->Unmap(0,&empty);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 ray_options{};hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&ray_options,sizeof(ray_options)));
    check(ray_options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1,"Native inline-ray fixture requires DXR 1.1");
    ComPtr<ID3D12Device5> ray_device;hr(device.As(&ray_device));ComPtr<ID3D12GraphicsCommandList4> ray_list;hr(list.As(&ray_list));
    const auto ray_original=compiler.compile(R"(
RaytracingAccelerationStructure scene:register(t3,space2);RWTexture2D<float4> target:register(u4,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){
 RayDesc ray;ray.Origin=float3((float2(p.xy)+.5)/float2(61,37)*2-1,-2);ray.Direction=float3(0,0,1);ray.TMin=0;ray.TMax=10;
 RayQuery<RAY_FLAG_CULL_NON_OPAQUE|RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;q.TraceRayInline(scene,0,255,ray);while(q.Proceed()){}
 float hit=q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?1:0;target[p.xy]=float4(hit,float(p.x)/64,float(p.y)/64,1);
})",L"cs_6_5");
    const auto ray_transform=arc::dx12::shader::coarse_compute(compiler.disassemble(ray_original.Get()),1,1,true);check(ray_transform.admitted,"Local inline ray query classification");
    auto ray_code=compiler.assemble(ray_transform.ir);std::array<ComPtr<ID3D12PipelineState>,2> ray_pipelines;IDxcBlob* ray_codes[]{ray_original.Get(),ray_code.Get()};
    for(unsigned i=0;i<2;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=mip_root.Get();p.CS={ray_codes[i]->GetBufferPointer(),ray_codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&ray_pipelines[i])));}wait_prepared(5);
    auto ray_buffer=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){auto d=bd;d.Width=size;d.Flags=flags;D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
    const float vertices[]{-.7f,-.7f,0,.7f,-.7f,0,0,.7f,0};auto vertices_buffer=ray_buffer(sizeof(vertices),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);
    hr(vertices_buffer->Map(0,&empty,&ptr));std::memcpy(ptr,vertices,sizeof(vertices));vertices_buffer->Unmap(0,nullptr);
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexBuffer={vertices_buffer->GetGPUVirtualAddress(),12};geometry.Triangles.VertexCount=3;geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};bottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;bottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;bottom.NumDescs=1;bottom.pGeometryDescs=&geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{};ray_device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom,&bottom_info);check(bottom_info.ResultDataMaxSizeInBytes>0,"BLAS prebuild");
    auto blas=ray_buffer(bottom_info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RAYTRACING_INSTANCE_DESC instance{};instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;instance.InstanceMask=255;instance.AccelerationStructure=blas->GetGPUVirtualAddress();
    auto instances=ray_buffer(sizeof(instance),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);hr(instances->Map(0,&empty,&ptr));std::memcpy(ptr,&instance,sizeof(instance));instances->Unmap(0,nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.NumDescs=1;top.InstanceDescs=instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_info{};ray_device->GetRaytracingAccelerationStructurePrebuildInfo(&top,&top_info);check(top_info.ResultDataMaxSizeInBytes>0,"TLAS prebuild");
    auto tlas=ray_buffer(top_info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto scratch=ray_buffer(std::max(bottom_info.ScratchDataSizeInBytes,top_info.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    desired={1,1,width,height};begin();D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_as{};build_as.Inputs=bottom;build_as.DestAccelerationStructureData=blas->GetGPUVirtualAddress();build_as.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();ray_list->BuildRaytracingAccelerationStructure(&build_as,0,nullptr);
    D3D12_RESOURCE_BARRIER as_barrier{};as_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&as_barrier);
    build_as.Inputs=top;build_as.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();ray_list->BuildRaytracingAccelerationStructure(&build_as,0,nullptr);list->ResourceBarrier(1,&as_barrier);execute();
    D3D12_SHADER_RESOURCE_VIEW_DESC as_view{};as_view.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;as_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;as_view.RaytracingAccelerationStructure.Location=tlas->GetGPUVirtualAddress();device->CreateShaderResourceView(nullptr,&as_view,heap->GetCPUDescriptorHandleForHeapStart());
    for(unsigned mode=0;mode<(arc_mode?7u:4u);++mode){const bool coarse_rays=mode==2||mode==5;desired={mode==2?2u:1u,mode==2?2u:1u,width,height};
        if(mode>=4){const auto* setting=mode==4?L"neutral":mode==5?L"2x2":L"off";check(arc_mode(const_cast<wchar_t*>(setting))==0,"Inline ray policy switch");}
        if(mode<2||mode==4){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(1,policy.address(0));list->SetPipelineState(ray_pipelines[mode==4?0:mode].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
            transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        }else submit();
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT sx=coarse_rays?x&~1u:x,sy=coarse_rays?y&~1u:y;const float rx=(float(sx)+.5f)/width*2-1,ry=(float(sy)+.5f)/height*2-1;const bool hit=ry>=-.7f&&ry<=.7f&&std::abs(rx)<=(.7f-ry)*.5f;const Pixel expected{hit?1.f:0.f,float(sx)/64,float(sy)/64,1};Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Inline rays, coarsened output and rollback must match geometric CPU oracle");}readbacks[0]->Unmap(0,&empty);
    }
    if(arc_mode){
        std::string bindless_source=R"(
Texture2D<float4> sources[]:register(t0,space42);RWTexture2D<float4> targets[]:register(u0,space43);
cbuffer Choice:register(b9,space7){uint inputIndex;uint outputIndex;}
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){targets[outputIndex][p.xy]=sources[inputIndex][p.xy];}
)";
        auto bindless_code=compiler.compile(bindless_source);auto bindless_contract=arc::dx12::shader::coarse_compute(compiler.disassemble(bindless_code.Get()),1,1,true);
        check(bindless_contract.admitted,"Bindless shader requires runtime access proof");
        check(!arc::dx12::shader::coarse_compute(compiler.disassemble(bindless_code.Get()),2,2).admitted,"Static bindless transform must refuse missing output extent proof");
        auto varying=bindless_source;const auto where=varying.find("sources[inputIndex]");varying.replace(where,19,"sources[NonUniformResourceIndex(p.x%5)]");auto varying_code=compiler.compile(varying);
        std::array<ComPtr<ID3D12PipelineState>,2> bindless_pipelines;IDxcBlob* bindless_codes[]{bindless_code.Get(),varying_code.Get()};
        for(unsigned i=0;i<2;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=mip_root.Get();p.CS={bindless_codes[i]->GetBufferPointer(),bindless_codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&bindless_pipelines[i])));}wait_prepared(7);
        srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(textures[0].Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        for(unsigned mode=0;mode<7;++mode){const auto* setting=mode==0||mode==3?L"off":mode==1?L"neutral":L"2x2";check(arc_mode(const_cast<wchar_t*>(setting))==0,"Bindless policy switch");desired={1,1,width,height};const unsigned output_index=mode==0||mode==5?0:1;const auto prior_proof=proven();
            // Change both the uniform output index and the descriptor behind
            // it. A stale proof would wrongly admit the old full-size output.
            if(mode==6){auto narrow=td;narrow.Width=width/2;hp.Type=D3D12_HEAP_TYPE_DEFAULT;textures[2].Reset();hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&narrow,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[2])));auto destination=heap->GetCPUDescriptorHandleForHeapStart();destination.ptr+=6*increment;device->CreateUnorderedAccessView(textures[2].Get(),nullptr,&uav,destination);}
            if(mode==0||mode==1||mode>=4){begin();if(mode!=6)copy_in(textures[output_index+1].Get(),initial.Get());transition(textures[output_index+1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(2,heap->GetGPUDescriptorHandleForHeapStart());auto output_table=heap->GetGPUDescriptorHandleForHeapStart();output_table.ptr+=5*increment;list->SetComputeRootDescriptorTable(4,output_table);const UINT indices[]{mode==0?0u:4u,output_index};list->SetComputeRoot32BitConstants(3,2,indices,0);list->SetPipelineState(bindless_pipelines[mode==4?1:0].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
                transition(textures[output_index+1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[output_index].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[output_index+1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[output_index+1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
            }else submit();
            if(mode==1||mode>=4)await_proof(prior_proof,setting);
            D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[output_index]->Map(0,&range,&ptr));
            for(UINT y=0;y<height;++y)for(UINT x=0;x<(mode==6?width/2:width);++x){const bool reduced=mode==2||mode==5;const auto expected=array_pixel(reduced?x&~1u:x,reduced?y&~1u:y,mode==0?0:mode==4?x%5:4);Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Uniform bindless access, varying-index refusal and rollback must match each pixel");}readbacks[output_index]->Unmap(0,&empty);
        }
        // ExecuteIndirect resets only its changed root constants. A redundant
        // root-signature bind must preserve the other inherited tables.
        D3D12_INDIRECT_ARGUMENT_DESC indirect_desc[3]{};indirect_desc[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;indirect_desc[0].ConstantBufferView.RootParameterIndex=1;indirect_desc[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;indirect_desc[1].Constant={3,0,2};indirect_desc[2].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        struct IndirectData{UINT64 cbv;UINT indices[2];D3D12_DISPATCH_ARGUMENTS dispatch;};const IndirectData indirect_data{policy.address(0),{4,1},{(width+7)/8,(height+7)/8,1}};
        D3D12_COMMAND_SIGNATURE_DESC command_desc{sizeof(IndirectData),3,indirect_desc,0};ComPtr<ID3D12CommandSignature> command_signature;const auto signature_status=device->CreateCommandSignature(&command_desc,mip_root.Get(),IID_PPV_ARGS(&command_signature));
        if(FAILED(signature_status))for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T length{};hr(diagnostics->GetMessage(i,nullptr,&length));std::vector<char> storage(length);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(diagnostics->GetMessage(i,message,&length));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::cerr<<message->pDescription<<'\n';}hr(signature_status);
        auto indirect_buffer=ray_buffer(sizeof(IndirectData),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);hr(indirect_buffer->Map(0,&empty,&ptr));std::memcpy(ptr,&indirect_data,sizeof(indirect_data));indirect_buffer->Unmap(0,nullptr);
        check(arc_mode(const_cast<wchar_t*>(L"2x2"))==0,"Indirect inheritance policy");const auto before_indirect_proof=proven();desired={1,1,width,height};begin();
        for(unsigned i=1;i<3;++i)transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* inherited_heaps[]{heap.Get()};list->SetDescriptorHeaps(1,inherited_heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(2,heap->GetGPUDescriptorHandleForHeapStart());auto inherited_output=heap->GetGPUDescriptorHandleForHeapStart();inherited_output.ptr+=5*increment;list->SetComputeRootDescriptorTable(4,inherited_output);const UINT initial_indices[]{0,0};list->SetComputeRoot32BitConstants(3,2,initial_indices,0);list->SetPipelineState(bindless_pipelines[0].Get());
        list->ExecuteIndirect(command_signature.Get(),1,indirect_buffer.Get(),0,nullptr,0);list->SetComputeRootSignature(mip_root.Get());list->SetPipelineState(bindless_pipelines[0].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
        transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);transition(textures[2].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);execute();await_proof(before_indirect_proof,L"2x2");
        D3D12_RANGE inherited_range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&inherited_range,&ptr));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto expected=array_pixel(x&~1u,y&~1u,0);Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Indirect root-constant reset and inherited descriptor tables must survive transformation");}readbacks[0]->Unmap(0,&empty);
        // Opaque work remains conservative. Rebinding the same root afterwards
        // does not give ARC permission to erase unobserved native arguments.
        begin();transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->SetDescriptorHeaps(1,inherited_heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(2,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootDescriptorTable(4,inherited_output);const UINT opaque_indices[]{4,0};list->SetComputeRoot32BitConstants(3,2,opaque_indices,0);list->SetPipelineState(bindless_pipelines[0].Get());
        ray_list->BuildRaytracingAccelerationStructure(&build_as,0,nullptr);list->ResourceBarrier(1,&as_barrier);list->SetComputeRootSignature(mip_root.Get());list->SetPipelineState(bindless_pipelines[0].Get());list->Dispatch((width+7)/8,(height+7)/8,1);
        transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
        hr(readbacks[0]->Map(0,&inherited_range,&ptr));for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto expected=array_pixel(x,y,4);Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Opaque native root state must remain untouched even when coarsening is requested");}readbacks[0]->Unmap(0,&empty);
    }
    if(arc_mode){
        const auto packed_code=compiler.compile(R"(
ByteAddressBuffer packed:register(t8,space9);RWTexture2D<float4> target:register(u4,space3);
[numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){uint w,h;target.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
 float value=f16tof32(packed.Load((p.y*w+p.x)*4));target[p.xy]=float4(value,float(p.x)/64,float(p.y)/64,1);}
)");
        const auto packed_ir=compiler.disassemble(packed_code.Get());check(arc::dx12::shader::coarse_compute(packed_ir,1,1,true).admitted,"Readonly raw buffer and packed half conversion classification");
        ComPtr<ID3D12PipelineState> packed_pipeline;D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=mip_root.Get();pd.CS={packed_code->GetBufferPointer(),packed_code->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&packed_pipeline)));wait_prepared(8);
        auto packed=ray_buffer(UINT64(width)*height*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);hr(packed->Map(0,&empty,&ptr));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)static_cast<UINT*>(ptr)[y*width+x]=(x+y)%2?0x3800u:0x3400u;packed->Unmap(0,nullptr);
        for(unsigned mode=0;mode<4;++mode){const auto* setting=mode==0||mode==3?L"off":mode==1?L"neutral":L"2x2";check(arc_mode(const_cast<wchar_t*>(setting))==0,"Packed buffer policy switch");desired={1,1,width,height};
            if(mode<2){begin();copy_in(textures[1].Get(),initial.Get());transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootShaderResourceView(5,packed->GetGPUVirtualAddress());list->SetPipelineState(packed_pipeline.Get());list->Dispatch((width+7)/8,(height+7)/8,1);
                transition(textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[0].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[1].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);execute();
            }else submit();
            D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[0]->Map(0,&range,&ptr));
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT sx=mode==2?x&~1u:x,sy=mode==2?y&~1u:y;const Pixel expected{(sx+sy)%2?.5f:.25f,float(sx)/64,float(sy)/64,1};Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"Packed root-buffer SRV, dimensions and rollback must match CPU oracle");}readbacks[0]->Unmap(0,&empty);
        }
    }
    if(arc_mode){
        // Original SM5.1 bytecode remains the independent baseline. The worker
        // converts, normalizes and validates its dynamic DXIL variant itself.
        std::ifstream file(directory/"legacy-input.bin",std::ios::binary);std::vector<char> legacy{std::istreambuf_iterator<char>(file),{}};
        ComPtr<ID3D12PipelineState> legacy_pipeline;D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=mip_root.Get();pd.CS={legacy.data(),legacy.size()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&legacy_pipeline)));wait_prepared(9);
        hp.Type=D3D12_HEAP_TYPE_DEFAULT;textures[2].Reset();hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[2])));auto destination=heap->GetCPUDescriptorHandleForHeapStart();destination.ptr+=6*increment;device->CreateUnorderedAccessView(textures[2].Get(),nullptr,&uav,destination);
        for(unsigned mode=0;mode<4;++mode){const auto* setting=mode==0||mode==3?L"off":mode==1?L"neutral":L"2x2";check(arc_mode(const_cast<wchar_t*>(setting))==0,"DXBC conversion policy");desired={1,1,width,height};
            if(mode<2){begin();for(unsigned i=1;i<3;++i){copy_in(textures[i].Get(),initial.Get());transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
                ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(mip_root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetPipelineState(legacy_pipeline.Get());list->Dispatch((width+7)/8,(height+7)/8,1);
                for(unsigned i=1;i<3;++i){transition(textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION d{},s{};d.pResource=readbacks[i-1].Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=footprint;s.pResource=textures[i].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);}execute();
            }else submit();
            for(unsigned target=0;target<2;++target){D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readbacks[target]->Map(0,&range,&ptr));
                for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT sx=mode==2?x&~1u:x,sy=mode==2?y&~1u:y;auto p=array_pixel(sx,sy,sy/8);p.r+=array_pixel(sx,sy,(sx/8)%5).r;const Pixel expected=p.a?(target?Pixel{p.b,p.r,p.g,.5f}:Pixel{p.r*2,p.g+.125f,p.b*.5f,1}):(target?Pixel{}:clear);Pixel actual;std::memcpy(&actual,static_cast<char*>(ptr)+y*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(Pixel));check(std::memcmp(&actual,&expected,sizeof(Pixel))==0,"DXBC baseline, validated conversion, coarsening and cached rollback must match every pixel");}readbacks[target]->Unmap(0,&empty);
            }
        }
    }
    unsigned validation_errors=0;for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T size{};hr(diagnostics->GetMessage(i,nullptr,&size));std::vector<char> memory(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(memory.data());hr(diagnostics->GetMessage(i,message,&size));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';++validation_errors;}}
    check(validation_errors==0,"D3D12 validation failed");CloseHandle(event);
    if(arc_snapshot)check(arc_snapshot(nullptr)==0,"Final generic optimizer snapshot");
    std::ofstream report(directory/"summary.json");report<<"{\"hardware\":true,\"width\":61,\"height\":37,\"outputs\":2,\"neutral_bit_exact\":true,\"cached_list_rollback\":true,\"coarse_every_pixel_verified\":true,\"comparison_filter_verified\":true,\"zero_factor_bit_exact\":true,\"edge_protection_verified\":true,\"mip_sampling_verified\":true,\"inline_ray_cpu_oracle_verified\":true,\"unsafe_shader_rejections\":"<<rejected<<",\"debug_errors\":0}\n";
    std::cout<<"Neutral and 2x2 transformed native dispatches passed, every output verified; interned root="<<interned_root<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
