#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include "json.hpp"
using Microsoft::WRL::ComPtr;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void hr(HRESULT value,std::source_location where=std::source_location::current()){
    if(FAILED(value))throw std::runtime_error("DX12 call failed at line "+std::to_string(where.line())+": "+std::to_string(value));}
int passive_pso_case(const std::filesystem::path& output,const std::filesystem::path& probe_path,const std::filesystem::path& worker_path,const std::filesystem::path& compiler_path){
    check(std::filesystem::is_regular_file(probe_path)&&std::filesystem::is_regular_file(worker_path)&&std::filesystem::is_regular_file(compiler_path),"Fixture dependencies");
    SetEnvironmentVariableW(L"ARC_OPTIMIZER_WORKER",worker_path.c_str());SetEnvironmentVariableW(L"ARC_OPTIMIZER_COMPILER",compiler_path.c_str());
    const auto cache=(output/L"shader-cache").wstring();SetEnvironmentVariableW(L"ARC_OPTIMIZER_CACHE",cache.c_str());
    SetEnvironmentVariableW(L"ARC_OPTIMIZER_LAZY_COMPILE",L"0");
    const auto module=LoadLibraryW(probe_path.c_str());check(module,"Load probe");using Api=DWORD(WINAPI*)(void*);
    const auto initialize=reinterpret_cast<Api>(GetProcAddress(module,"ArcInitialize"));
    const auto passive=reinterpret_cast<Api>(GetProcAddress(module,"ArcUsePassiveMode"));
    const auto resume=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalCompute"));
    const auto snapshot=reinterpret_cast<Api>(GetProcAddress(module,"ArcSnapshot"));
    check(initialize&&passive&&resume&&snapshot,"Probe controls");auto report=(output/L"arc.json").wstring();
    check(initialize(report.data())==0&&passive(nullptr)==0,"Enter passive capture");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC root_desc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> root_blob;hr(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));
    ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    HMODULE compiler_module=LoadLibraryExW(compiler_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(compiler_module,"Load DXC");auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler_module,"DxcCreateInstance"));check(create,"DXC factory");
    ComPtr<IDxcLibrary> library;ComPtr<IDxcCompiler> compiler;hr(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)));hr(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));
    auto compile=[&](const std::string& source,const wchar_t* entry,const wchar_t* model){
        ComPtr<IDxcBlobEncoding> text;hr(library->CreateBlobWithEncodingOnHeapCopy(source.data(),UINT(source.size()),CP_UTF8,&text));
        ComPtr<IDxcOperationResult> operation;hr(compiler->Compile(text.Get(),L"passive",entry,model,nullptr,0,nullptr,0,nullptr,&operation));
        HRESULT compiled{};hr(operation->GetStatus(&compiled));hr(compiled);ComPtr<IDxcBlob> shader;hr(operation->GetResult(&shader));return shader;};
    auto shader=compile("RWTexture2D<float4> output:register(u0); [numthreads(8,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=float4(p.xy,0,1);}",L"MainCS",L"cs_6_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};pso_desc.pRootSignature=root.Get();pso_desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> same_pso;hr(device->CreateComputePipelineState(&pso_desc,IID_PPV_ARGS(&same_pso)));
    auto vs=compile("float4 MainVS(uint id:SV_VertexID):SV_Position{return float4(float(id&1),float((id>>1)&1),0,1);}",L"MainVS",L"vs_6_0");
    auto ps=compile("float4 MainPS():SV_Target{return float4(1,0,0,1);}",L"MainPS",L"ps_6_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=root.Get();graphics.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    graphics.PS={ps->GetBufferPointer(),ps->GetBufferSize()};graphics.SampleMask=UINT_MAX;
    graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable=TRUE;graphics.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;graphics.NumRenderTargets=1;
    graphics.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;graphics.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> pixel_pso;hr(device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&pixel_pso)));
    auto stream_shader=compile("[numthreads(16,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){}",L"MainCS",L"cs_6_0");
    struct alignas(void*) RootPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;ID3D12RootSignature* value;};
    struct alignas(void*) CsPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_SHADER_BYTECODE value;};
    struct Stream{RootPart root;CsPart cs;} stream_data{
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,root.Get()},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,{stream_shader->GetBufferPointer(),stream_shader->GetBufferSize()}}};
    D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(stream_data),&stream_data};ComPtr<ID3D12Device2> device2;
    hr(device.As(&device2));ComPtr<ID3D12PipelineState> stream_pso;
    hr(device2->CreatePipelineState(&stream,IID_PPV_ARGS(&stream_pso)));
    auto input_vs=compile("float4 MainVS(float3 p:POSITION):SV_Position{return float4(p,1);}",L"MainVS",L"vs_6_0");
    char semantic[]="POSITION";D3D12_INPUT_ELEMENT_DESC input_element{semantic,0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    D3D12_INPUT_LAYOUT_DESC input_layout{&input_element,1};
    struct alignas(void*) InputPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_INPUT_LAYOUT_DESC value;};
    struct alignas(void*) RasterPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_RASTERIZER_DESC value;};
    struct alignas(void*) BlendPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_BLEND_DESC value;};
    struct alignas(void*) MaskPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;UINT value;};
    struct alignas(void*) TopologyPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_PRIMITIVE_TOPOLOGY_TYPE value;};
    struct alignas(void*) FormatsPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;D3D12_RT_FORMAT_ARRAY value;};
    struct alignas(void*) SamplesPart{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;DXGI_SAMPLE_DESC value;};
    struct GraphicsStream{RootPart root;CsPart vs;CsPart ps;InputPart input;RasterPart raster;BlendPart blend;MaskPart mask;
        TopologyPart topology;FormatsPart formats;SamplesPart samples;} graphics_stream{};
    graphics_stream.root={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,root.Get()};
    graphics_stream.vs={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,{input_vs->GetBufferPointer(),input_vs->GetBufferSize()}};
    graphics_stream.ps={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,{ps->GetBufferPointer(),ps->GetBufferSize()}};
    graphics_stream.input={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,input_layout};
    graphics_stream.raster={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,graphics.RasterizerState};
    graphics_stream.blend={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,graphics.BlendState};
    graphics_stream.mask={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT_MAX};
    graphics_stream.topology={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
    D3D12_RT_FORMAT_ARRAY formats{};formats.NumRenderTargets=1;formats.RTFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics_stream.formats={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,formats};
    graphics_stream.samples={D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,{1,0}};
    D3D12_PIPELINE_STATE_STREAM_DESC graphics_stream_desc{sizeof(graphics_stream),&graphics_stream};
    ComPtr<ID3D12PipelineState> pixel_stream_pso;hr(device2->CreatePipelineState(&graphics_stream_desc,IID_PPV_ARGS(&pixel_stream_pso)));
    semantic[0]='X';
    // The application drops another root and PSO before resume. The passive
    // queue must keep them valid through replay, then release both identities.
    D3D12_ROOT_PARAMETER extra_parameter{};extra_parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    extra_parameter.Constants={7,0,1};D3D12_ROOT_SIGNATURE_DESC extra_desc{1,&extra_parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> extra_blob;hr(D3D12SerializeRootSignature(&extra_desc,D3D_ROOT_SIGNATURE_VERSION_1,&extra_blob,nullptr));
    ComPtr<ID3D12RootSignature> released_root;hr(device->CreateRootSignature(0,extra_blob->GetBufferPointer(),extra_blob->GetBufferSize(),IID_PPV_ARGS(&released_root)));
    auto released_shader=compile("[numthreads(4,1,1)]void MainCS(uint3 p:SV_DispatchThreadID){}",L"MainCS",L"cs_6_0");
    auto released_desc=pso_desc;released_desc.pRootSignature=released_root.Get();released_desc.CS={released_shader->GetBufferPointer(),released_shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> released_pso;hr(device->CreateComputePipelineState(&released_desc,IID_PPV_ARGS(&released_pso)));
    released_pso.Reset();released_root.Reset();released_shader.Reset();
    shader.Reset();vs.Reset();ps.Reset();stream_shader.Reset();input_vs.Reset();compiler.Reset();library.Reset();FreeLibrary(compiler_module);
    check(snapshot(nullptr)==0,"Passive snapshot");{
        std::ifstream file(output/L"arc.json");const auto json=nlohmann::json::parse(file);
        check(json.at("cheap_observer").at("retained_pso_and_roots").get<unsigned>()>=7,"Passive root and PSOs retained");
        check(json.at("optimizer_coverage").at("observed_compute_pipelines").get<unsigned>()==0,"No passive analysis");}
    auto* identity=same_pso.Get();wchar_t neutral[]=L"neutral";check(resume(neutral)==0,"Resume ANALYZE");
    const auto deadline=GetTickCount64()+15000;bool analyzed=false;
    while(GetTickCount64()<deadline){Sleep(50);check(snapshot(nullptr)==0,"Resume snapshot");
        std::ifstream file(output/L"arc.json");const auto json=nlohmann::json::parse(file);
        const auto& observer=json.at("cheap_observer"),&optimizer=json.at("optimizer");
        if(observer.at("replayed_objects").get<unsigned>()>=7&&observer.at("retained_pso_and_roots").get<unsigned>()==0&&
            json.at("optimizer_coverage").at("observed_compute_pipelines").get<unsigned>()>=2&&
            json.at("optimizer_coverage").at("observed_root_signatures").get<unsigned>()>=1&&
            json.at("pixel_optimizer").at("tracked_pipelines").get<unsigned>()>=1&&
            json.at("gpu_profile").at("live_pipelines").get<unsigned>()>=4&&optimizer.at("prepared").get<unsigned>()>=1){analyzed=true;break;}}
    check(analyzed&&same_pso.Get()==identity,"Same passive PSO analyzed without recreation");
    std::cout<<"Passive PSO resumed and analyzed without recreation\n";return 0;
}
int wmain(int argc,wchar_t** argv)try{
    if(argc==6&&std::wstring(argv[1])==L"--passive-pso")return passive_pso_case(argv[2],argv[3],argv[4],argv[5]);
    if(argc==5&&std::wstring(argv[1]).starts_with(L"controlled-proof:")){std::ofstream(std::filesystem::path(std::wstring(argv[3])+L".entered"))<<GetCurrentProcessId();Sleep(10000);return 3;}
    check(argc>=3,"--parent/--render <evidence directory> [late DLL]");const std::filesystem::path output=argv[2];
    if(std::wstring(argv[1]).starts_with(L"--parent")){
        const bool suspended=std::wstring(argv[1])==L"--parent-suspended";
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto command=L"\""+std::wstring(exe)+L"\" --render \""+output.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
        check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|(suspended?CREATE_SUSPENDED:0),nullptr,nullptr,&startup,&child),"create renderer");
        if(suspended){check(!std::filesystem::exists(output/L"renderer.json"),"suspended child ran early");check(ResumeThread(child.hThread)==1,"original suspend count was not preserved");}
        std::ofstream(output/L"parent.json")<<"{\"parent_pid\":"<<GetCurrentProcessId()<<",\"renderer_pid\":"<<child.dwProcessId<<"}";
        CloseHandle(child.hThread);CloseHandle(child.hProcess);return 0; // bootstrap exits immediately
    }
    const bool m1=std::wstring(argv[1])==L"--m1-observer";
    const bool overflow=std::wstring(argv[1])==L"--profile-overflow";
    const bool before=GetModuleHandleW(L"arc-dx12-probe.dll")!=nullptr;
    const bool resize_test=std::wstring(argv[1])==L"--resize";
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_ROOT_SIGNATURE_DESC root_desc{};ComPtr<ID3DBlob> root_blob;hr(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));
    ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char source[]="[numthreads(1,1,1)] void main() {}";ComPtr<ID3DBlob> shader;hr(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,nullptr));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};pso_desc.pRootSignature=root.Get();pso_desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;hr(device->CreateComputePipelineState(&pso_desc,IID_PPV_ARGS(&pso)));
    // Keep >512 distinct live PSOs: an early fixed catalog used to stop here.
    std::vector<ComPtr<ID3D12RootSignature>> extra_roots;
    std::vector<ComPtr<ID3D12PipelineState>> extra_pipelines;
    for(unsigned i=0;i<600;++i){D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants={0,i+1,1};
        D3D12_ROOT_SIGNATURE_DESC description{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob;hr(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));
        ComPtr<ID3D12RootSignature> r;hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&r)));
        auto d=pso_desc;d.pRootSignature=r.Get();ComPtr<ID3D12PipelineState> p;hr(device->CreateComputePipelineState(&d,IID_PPV_ARGS(&p)));extra_roots.push_back(r);extra_pipelines.push_back(p);}
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=1025;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ComPtr<ID3D12DescriptorHeap> source_heap,destination_heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&source_heap)));hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&destination_heap)));
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> sources;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
    for(unsigned i=0;i<1025;++i){auto handle=source_heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(i)*stride;device->CreateShaderResourceView(nullptr,&view,handle);sources.push_back(handle);}
    const auto destination=destination_heap->GetCPUDescriptorHandleForHeapStart();UINT count=1025;
    device->CopyDescriptors(1,&destination,&count,count,sources.data(),nullptr,hd.Type);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<IDXGIFactory2> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ArcConnectionFixture";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);check(window,"hidden window");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=2;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap));
    if(argc==4){const auto dll=LoadLibraryW(argv[3]);check(dll,"late load");const auto initialize=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll,"ArcInitialize"));auto file=(output/L"arc.json").wstring();check(initialize&&initialize(file.data())==0,"late initialize");}
    using Api=DWORD(WINAPI*)(void*);Api stop_optimizer{},passive_mode{},compute_mode{};unsigned long long initial_novelty{};
    if(m1){const auto dll=GetModuleHandleW(L"arc-dx12-probe.dll");stop_optimizer=reinterpret_cast<Api>(GetProcAddress(dll,"ArcStopOptimizer"));passive_mode=reinterpret_cast<Api>(GetProcAddress(dll,"ArcUsePassiveMode"));compute_mode=reinterpret_cast<Api>(GetProcAddress(dll,"ArcExperimentalCompute"));check(stop_optimizer&&passive_mode&&compute_mode,"M1 controls");
        ComPtr<ID3D12RootSignature> observed_root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&observed_root)));D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=observed_root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};ComPtr<ID3D12PipelineState> observed_pso;hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&observed_pso)));
        wchar_t cache_path[32768]{};GetEnvironmentVariableW(L"ARC_OPTIMIZER_CACHE",cache_path,32768);DWORD child_pid=0;
        for(unsigned attempt=0;attempt<400&&!child_pid;++attempt){for(const auto& e:std::filesystem::recursive_directory_iterator(cache_path))if(e.path().extension()==L".entered"){std::ifstream f(e.path());f>>child_pid;break;}if(!child_pid)Sleep(5);}
        check(child_pid,"Bounded compiler fixture actually started");HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,child_pid);check(child,"Compiler process handle");check(stop_optimizer(nullptr)==0,"User stop cancels analysis");check(WaitForSingleObject(child,2000)==WAIT_OBJECT_0,"Owned compiler stops after cancellation");CloseHandle(child);
        check(passive_mode(nullptr)==0,"Enter cheap observation after stop");
    }
    ComPtr<ID3D12PipelineState> alternate;
    if(overflow){const char extra[]="[numthreads(2,1,1)]void main(){}";ComPtr<ID3DBlob> code;hr(D3DCompile(extra,sizeof(extra)-1,nullptr,nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,nullptr));D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&alternate)));}
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    for(unsigned i=1;i<=(overflow?1800u:100u);++i){if(resize_test&&i==40)hr(swap->ResizeBuffers(2,4,4,desc.Format,0));if(resize_test&&i==70){swap.Reset();hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap));}
        if(m1&&i==30){D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};ComPtr<ID3D12PipelineState> hint;hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&hint)));}
        if(m1&&i==50){wchar_t off[]=L"off";check(compute_mode(off)==0,"Rearm discovery without restarting process");}
        hr(allocator->Reset());hr(list->Reset(allocator.Get(),pso.Get()));list->SetComputeRootSignature(root.Get());if(overflow){for(unsigned j=0;j<260;++j){list->SetPipelineState(j%2?pso.Get():alternate.Get());list->Dispatch(1,1,1);}}else list->Dispatch(1,1,1);hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),i));hr(fence->SetEventOnCompletion(i,event));check(WaitForSingleObject(event,2000)==WAIT_OBJECT_0,"GPU completion");hr(swap->Present(0,0));Sleep(25);}
    const auto dll=GetModuleHandleW(L"arc-dx12-probe.dll");check(dll,"ARC loaded");const auto snapshot=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll,"ArcSnapshot"));check(snapshot&&snapshot(nullptr)==0,"snapshot");
    if(overflow){std::ifstream file(output/L"arc.json");const auto metrics=nlohmann::json::parse(file);check(metrics.value("render_hooks_passive",false),"Unsupported profile must become passive");const auto session=metrics.at("automatic_session");check(session.value("phase",std::string{})=="observing"&&session.value("observer_active",false),"No-actuator controller must retain cheap observation");check(session.value("deep_discovery_idle",false),"Deep discovery must remain in cooldown");}
    if(m1){std::ifstream file(output/L"arc.json");const auto metrics=nlohmann::json::parse(file);const auto observer=metrics.at("cheap_observer");check(observer.at("presents").get<unsigned>()>=100&&observer.at("novelty").get<unsigned>()>=2,"Cheap observer continues during idle");check(observer.at("correctness_epoch").get<unsigned>()>1,"Observation gaps invalidate critical evidence");check(!metrics.value("render_hooks_passive",true)&&metrics.at("optimizer").value("discovery_enabled",false),"Discovery resumed through runtime");check(stop_optimizer(nullptr)==0,"Final discovery stop");}
    nlohmann::json arguments=nlohmann::json::array();for(int i=0;i<argc;++i){const auto size=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(size,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),size,nullptr,nullptr);value.pop_back();arguments.push_back(value);}
    char appid[128]{};GetEnvironmentVariableA("SteamAppId",appid,128);
    std::ofstream(output/L"renderer.json")<<nlohmann::json{{"pid",GetCurrentProcessId()},{"arc_before_main",before},{"device_healthy",device->GetDeviceRemovedReason()==S_OK},{"arguments",arguments},{"steam_app_id",appid}}.dump(2);
    CloseHandle(event);DestroyWindow(window);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
