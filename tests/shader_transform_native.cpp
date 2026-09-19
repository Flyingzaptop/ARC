#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "generic_shader_transform.hpp"
#include "generic_gpu_control.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void hr(HRESULT v){if(FAILED(v))throw std::runtime_error("HRESULT "+std::to_string(v));}
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
    ComPtr<IDxcBlob> compile(const std::string& s){auto b=blob(s);ComPtr<IDxcOperationResult> op;hr(compiler->Compile(b.Get(),L"fixture",L"MainCS",L"cs_6_0",nullptr,0,nullptr,0,nullptr,&op));return result(op.Get());}
    std::string disassemble(IDxcBlob* b){ComPtr<IDxcBlobEncoding> text;hr(compiler->Disassemble(b,&text));return {static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize()};}
    ComPtr<IDxcBlob> assemble(const std::string& s){auto b=blob(s);ComPtr<IDxcOperationResult> op;hr(assembler->AssembleToContainer(b.Get(),&op));auto binary=result(op.Get());op.Reset();hr(validator->Validate(binary.Get(),DxcValidatorFlags_InPlaceEdit,&op));return result(op.Get());}
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
    if(c.a==0){output_aux[pixel.xy]=0;return;}
    output_color[pixel.xy]=float4(c.r*2,c.g+0.125,c.b*0.5,1);
    output_aux[pixel.xy]=float4(c.b,c.r,c.g,0.5);
})";
    auto original=compiler.compile(source);auto ir=compiler.disassemble(original.Get());
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

    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    using Api=DWORD(WINAPI*)(void*);Api arc_mode{},arc_snapshot{};auto arc_path=(directory/L"arc.json").wstring();
    if(argc==4){HMODULE probe=LoadLibraryW(argv[3]);check(probe!=nullptr,"Load generic optimizer DLL");auto init=reinterpret_cast<Api>(GetProcAddress(probe,"ArcInitialize"));auto lean=reinterpret_cast<Api>(GetProcAddress(probe,"ArcUseLeanMode"));arc_mode=reinterpret_cast<Api>(GetProcAddress(probe,"ArcExperimentalCompute"));arc_snapshot=reinterpret_cast<Api>(GetProcAddress(probe,"ArcSnapshot"));check(init&&lean&&arc_mode&&arc_snapshot&&init(arc_path.data())==0&&lean(nullptr)==0,"Initialize generic optimizer");}
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
    D3D12_DESCRIPTOR_RANGE ranges[3]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,3,2,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,4,3,5},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,7,3,6}};
    D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={3,ranges};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[1].Descriptor={0,controlled.control_space};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;
    ComPtr<ID3DBlob> root_blob,error;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error));ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    IDxcBlob* codes[]{original.Get(),neutral_code.Get(),coarse_code.Get(),controlled_code.Get()};
    for(unsigned i=0;i<4;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={codes[i]->GetBufferPointer(),codes[i]->GetBufferSize()};hr(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipelines[i])));}
    if(arc_snapshot){const auto deadline=GetTickCount64()+10000;bool ready=false;while(GetTickCount64()<deadline){check(arc_snapshot(nullptr)==0,"Optimizer snapshot");std::ifstream file(arc_path);std::string json{std::istreambuf_iterator<char>(file),{}};if(json.find("\"prepared\":1")!=json.npos){ready=true;break;}Sleep(20);}check(ready,"Generic worker must prepare a real shader variant");}
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
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT sx=x_rates[mode]==2?x&~1u:x,sy=y_rates[mode]==2?y&~1u:y;const auto p=input[sy*width+sx];const Pixel expected=p.a?(target?Pixel{p.b,p.r,p.g,.5f}:Pixel{p.r*2,p.g+.125f,p.b*.5f,1}):(target?Pixel{}:clear);check(std::memcmp(&pixels[y*width+x],&expected,sizeof(Pixel))==0,"Every output pixel including edges and conditional stores must match");}
        }
    }
    unsigned validation_errors=0;for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T size{};hr(diagnostics->GetMessage(i,nullptr,&size));std::vector<char> memory(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(memory.data());hr(diagnostics->GetMessage(i,message,&size));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';++validation_errors;}}
    check(validation_errors==0,"D3D12 validation failed");CloseHandle(event);
    if(arc_snapshot)check(arc_snapshot(nullptr)==0,"Final generic optimizer snapshot");
    std::ofstream report(directory/"summary.json");report<<"{\"hardware\":true,\"width\":61,\"height\":37,\"outputs\":2,\"neutral_bit_exact\":true,\"cached_list_rollback\":true,\"coarse_every_pixel_verified\":true,\"unsafe_shader_rejections\":"<<rejected<<",\"debug_errors\":0}\n";
    std::cout<<"Neutral and 2x2 transformed native dispatches passed, every output verified\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
