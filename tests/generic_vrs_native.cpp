#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind,class T>struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Kind};T value{};};
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("HRESULT "+std::to_string(r));}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3||argc==4,"DLL and fresh output directory required; optional --stream-renderpass[-lean][-indirect]");const bool stream_pass=argc==4;const std::wstring options=argc==4?argv[3]:L"";const bool lean_mode=options.find(L"lean")!=std::wstring::npos,indirect=options.find(L"indirect")!=std::wstring::npos;std::filesystem::path root=std::filesystem::absolute(argv[2]);
    check(!std::filesystem::exists(root),"Fresh directory required");std::filesystem::create_directories(root);
    ComPtr<ID3D12Debug> debug; if(options.find(L"nodebug")==std::wstring::npos&&SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    auto module=LoadLibraryW(argv[1]);check(module!=nullptr,"Load observer");using Api=DWORD(WINAPI*)(void*);
    auto init=reinterpret_cast<Api>(GetProcAddress(module,"ArcInitialize"));auto mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalVrs"));auto snapshot=reinterpret_cast<Api>(GetProcAddress(module,"ArcSnapshot"));
    auto metrics=(root/L"runtime.json").wstring();check(init&&mode&&snapshot&&init(metrics.data())==0,"Initialize mirror");
    Api cpu_mode{};
    if(options.find(L"cpu")!=std::wstring::npos){cpu_mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalCpuState"));wchar_t off[]=L"off";check(cpu_mode&&cpu_mode(off)==0,"Independent unoptimized CPU reference");}
    if(lean_mode){auto lean=reinterpret_cast<Api>(GetProcAddress(module,"ArcUseLeanMode"));check(lean&&lean(nullptr)==0,"Disable detailed observer independently of mirror");}
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps{};hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&caps,sizeof(caps)));
    if(caps.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_2)return 77;
    ComPtr<ID3D12InfoQueue> diagnostics;device.As(&diagnostics);
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    const char* shader=R"(
    struct V {float4 p:SV_Position;};
    V vs(uint id:SV_VertexID){V o;float2 p=float2((id<<1)&2,id&2);o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}
    float4 ps(V i):SV_Target {
      float2 uv=i.p.xy/float2(1280,720);float3 x=float3(uv,.25);
      [loop]for(uint k=0;k<192;k++)x=sin(x.yzx*1.13+x*.017+float(k)*.001);
      return float4(float3(uv.x,uv.y,.3)+x*.003,1);
    })";
    ComPtr<ID3DBlob> vs,ps,error;hr(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));
    hr(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> signature;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
    ComPtr<ID3D12RootSignature> roots;hr(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&roots)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=roots.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> pso;
    if(stream_pass){
        struct Stream {
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> sample;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> targets;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        } stream;
        stream.root.value=roots.Get();stream.vs.value=pd.VS;stream.ps.value=pd.PS;stream.raster.value=pd.RasterizerState;stream.blend.value=pd.BlendState;stream.depth.value=pd.DepthStencilState;
        stream.sample.value=pd.SampleDesc;stream.targets.value.NumRenderTargets=1;stream.targets.value.RTFormats[0]=pd.RTVFormats[0];stream.topology.value=pd.PrimitiveTopologyType;
        ComPtr<ID3D12Device2> modern;hr(device.As(&modern));D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};hr(modern->CreatePipelineState(&desc,IID_PPV_ARGS(&pso)));
    }else hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)));
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=1280;td.Height=720;
    td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size{};device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&size);
    auto readback=[&](UINT64 bytes){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;};
    auto pixels=readback(size),times=readback(16);
    ComPtr<ID3D12CommandSignature> indirect_signature;ComPtr<ID3D12Resource> indirect_arguments;
    if(indirect){D3D12_INDIRECT_ARGUMENT_DESC arg{};arg.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS),1,&arg,0};
        hr(device->CreateCommandSignature(&signature_desc,nullptr,IID_PPV_ARGS(&indirect_signature)));
        D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizeof(D3D12_DRAW_ARGUMENTS);buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indirect_arguments)));
        void* mapped{};D3D12_RANGE empty{0,0};hr(indirect_arguments->Map(0,&empty,&mapped));*static_cast<D3D12_DRAW_ARGUMENTS*>(mapped)={3,1,0,0};indirect_arguments->Unmap(0,nullptr);
    }
    D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=2;ComPtr<ID3D12QueryHeap> query;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&query)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pso.Get(),IID_PPV_ARGS(&commands)));hr(commands->Close());
    auto record=[&](bool unsupported,bool late_unsupported=false,bool app_coarse=false,bool change_rate_after_draw=false){hr(allocator->Reset());hr(commands->Reset(allocator.Get(),pso.Get()));
        if(unsupported)commands->ClearState(pso.Get()); // deliberately excluded mirror API
        commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);commands->SetGraphicsRootSignature(roots.Get());commands->SetPipelineState(pso.Get());
        D3D12_VIEWPORT viewport{0,0,1280,720,0,1};D3D12_RECT scissor{0,0,1280,720};commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
        if(options.find(L"cpu")!=std::wstring::npos){
            const float blend[]{1,1,1,1};
            for(unsigned repeat=0;repeat<8;++repeat){
                commands->SetPipelineState(pso.Get());commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
                commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->OMSetBlendFactor(blend);commands->OMSetStencilRef(0);
            }
            auto reduced_viewport=viewport;reduced_viewport.Width=320;auto clipped=scissor;clipped.right=320;
            commands->RSSetViewports(1,&reduced_viewport);commands->RSSetScissorRects(1,&clipped);
            commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
        }
        ComPtr<ID3D12GraphicsCommandList4> pass;
        if(stream_pass){hr(commands.As(&pass));D3D12_RENDER_PASS_RENDER_TARGET_DESC target_desc{};target_desc.cpuDescriptor=rtv;target_desc.BeginningAccess.Type=D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;target_desc.EndingAccess.Type=D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;pass->BeginRenderPass(1,&target_desc,nullptr,options.find(L"suspend")!=std::wstring::npos?D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS:D3D12_RENDER_PASS_FLAG_NONE);}
        else commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ComPtr<ID3D12GraphicsCommandList5> v5;hr(commands.As(&v5));v5->RSSetShadingRate(app_coarse?D3D12_SHADING_RATE_2X1:D3D12_SHADING_RATE_1X1,nullptr);
        if(indirect)commands->ExecuteIndirect(indirect_signature.Get(),1,indirect_arguments.Get(),0,nullptr,0);else commands->DrawInstanced(3,1,0,0);
        if(change_rate_after_draw){v5->RSSetShadingRate(D3D12_SHADING_RATE_2X1,nullptr);commands->DrawInstanced(3,1,0,0);}
        if(pass){pass->EndRenderPass();if(options.find(L"suspend")!=std::wstring::npos){D3D12_RENDER_PASS_RENDER_TARGET_DESC resumed{};resumed.cpuDescriptor=rtv;resumed.BeginningAccess.Type=D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;resumed.EndingAccess.Type=D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;pass->BeginRenderPass(1,&resumed,nullptr,D3D12_RENDER_PASS_FLAG_RESUMING_PASS);pass->EndRenderPass();}}if(late_unsupported)commands->ClearState(pso.Get());commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);commands->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={target.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};commands->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=pixels.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);commands->ResourceBarrier(1,&b);hr(commands->Close());};
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 fence_value=0,frequency{};hr(queue->GetTimestampFrequency(&frequency));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");
    auto run=[&](const wchar_t* name){std::vector<double> ms;for(int i=0;i<9;++i){ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),++fence_value));hr(fence->SetEventOnCompletion(fence_value,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU completion timeout");
            void* data{};D3D12_RANGE range{0,16},empty{0,0};hr(times->Map(0,&range,&data));auto* t=static_cast<UINT64*>(data);const double value=double(t[1]-t[0])*1000.0/double(frequency);times->Unmap(0,&empty);if(i>=2)ms.push_back(value);}
        void* data{};D3D12_RANGE range{0,static_cast<SIZE_T>(size)},empty{0,0};hr(pixels->Map(0,&range,&data));std::vector<unsigned char> result(1280*720*4);
        for(UINT y=0;y<720;++y)memcpy(result.data()+y*1280*4,static_cast<char*>(data)+y*footprint.Footprint.RowPitch,1280*4);pixels->Unmap(0,&empty);
        std::ofstream image(root/(std::wstring(name)+L".rgba"),std::ios::binary);image.write(reinterpret_cast<const char*>(result.data()),result.size());
        std::ofstream json(root/(std::wstring(name)+L".json"));json<<"{\"width\":1280,\"height\":720,\"gpu_ms\":[";for(std::size_t i=0;i<ms.size();++i){if(i)json<<',';json<<ms[i];}json<<"]}";
        return result;};
    record(false);auto baseline=run(L"baseline");
    if(options.find(L"cpu")!=std::wstring::npos){
        cpu_mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalCpuState"));
        wchar_t on[]=L"on";check(cpu_mode&&cpu_mode(on)==0,"Enable exact native state cache");
        record(false);check(run(L"cpu-state-exact")==baseline,"CPU state optimization must preserve every pixel");
    }
    if(options.find(L"profile")!=std::wstring::npos){
        auto request=reinterpret_cast<Api>(GetProcAddress(module,"ArcRequestGpuProfile"));auto stop=reinterpret_cast<Api>(GetProcAddress(module,"ArcStopGpuProfile"));
        auto path=(root/L"profile.json").wstring();auto argument=L"1|"+path;check(request&&stop&&request(argument.data())==0,"Request graphics cost profile");
        record(false);check(run(L"profiled-reference")==baseline,"Timestamp instrumentation must preserve exact raster pixels");check(stop(nullptr)==0,"Stop graphics profile");
        for(int i=0;i<500&&!std::filesystem::exists(path);++i)Sleep(10);std::ifstream file(path);std::string result((std::istreambuf_iterator<char>(file)),{});
        check(result.find("\"faults\":0")!=std::string::npos&&(options.find(L"suspend")!=std::wstring::npos?result.find("\"unsupported_segments\":1")!=std::string::npos:result.find("\"kind\":\"raster_color\"")!=std::string::npos),"Raster/render-pass timing must complete without faults");
    }
    if(options.find(L"passive")!=std::wstring::npos){auto passive=reinterpret_cast<Api>(GetProcAddress(module,"ArcUsePassiveMode"));check(passive&&passive(nullptr)==0,"Disable render hooks for passive baseline");check(run(L"passive-original")==baseline,"Removing instrumentation must preserve cached original pixels");}
    wchar_t enable[]=L"2x2",disable[]=L"off";check(mode(enable)==0,"All mirror hook coverage required before enabling");record(false);auto modified=run(L"modified");
    check(mode(disable)==0,"Disable mirror");auto restored=run(L"restored-cached-list");
    check(baseline==restored,"Rollback must restore exact pixels without rerecording the cached original list");
    if(diagnostics){for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T size=0;diagnostics->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());hr(diagnostics->GetMessage(i,message,&size));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 validation error");}}}
    snapshot(nullptr);
    check(modified!=baseline,"Experiment must actually change shader frequency/output");
    check(mode(enable)==0,"Enable negative coverage test");record(true);auto declined=run(L"unsupported-original");check(declined==baseline,"Unknown command must select original execution");
    record(false);check(run(L"qualification-original")==baseline,"Previously rejected command must first requalify without modification");
    record(false);check(run(L"requalified-modified")!=baseline,"Changed command contents must become eligible after requalification");
    check(mode(disable)==0,"Final restore");check(run(L"final-cached-restore")==baseline,"Final cached command replay must retain original pixels");
    check(mode(enable)==0,"Enable passive rollback test");record(false);check(run(L"before-passive")!=baseline,"Active map before passive rollback");
    auto passive=reinterpret_cast<Api>(GetProcAddress(module,"ArcUsePassiveMode"));check(passive&&passive(nullptr)==0,"Passive mode transition");
    check(run(L"passive-cached-restore")==baseline,"Passive mode must neutralize previously modified cached lists");
    check(mode(enable)==0,"Reactivate after passive");record(false);check(run(L"passive-reactivated")!=baseline,"Reactivation must change pixels");
    ComPtr<ID3D12CommandQueue> second;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&second)));queue.Swap(second);
    check(mode(disable)==0,"Disable before queue migration");check(run(L"second-queue-restored")==baseline,"Cached list migration to another queue retains rollback");
    check(mode(enable)==0,"Late unsupported operation");record(false,true);check(run(L"late-unsupported")==baseline,"Unsupported command after injected draw must neutralize entire recording");
    record(false);record(false);check(run(L"after-late-requalification")!=baseline,"Requalification after late unsupported operation");
    check(mode(disable)==0,"Reference application VRS");record(false,false,true);auto application_rate=run(L"application-rate-reference");
    check(mode(enable)==0,"Preserve application VRS");record(false,false,true);check(run(L"application-rate-preserved")==application_rate,"Non-default application shading must be left untouched");
    record(false,false,false,true);check(run(L"application-rate-after-controlled-draw")==application_rate,"Changing application VRS after a controlled draw must restore the native state first");
    // The application is allowed to Reset with another allocator while an old
    // generation is in flight. Neither recording nor policy rollback may wait
    // on the CPU for the GPU gate that this very thread will signal later.
    check(mode(enable)==0,"Enable in-flight generation test");record(false);
    ComPtr<ID3D12Fence> gate;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    hr(queue->Wait(gate.Get(),1));ID3D12CommandList* queued[]{commands.Get()};queue->ExecuteCommandLists(1,queued);
    auto old_allocator=allocator;auto old_pixels=pixels;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));pixels=readback(size);
    record(false);check(mode(disable)==0,"Disable queued generation");queue->ExecuteCommandLists(1,queued);
    hr(gate->Signal(1));hr(queue->Signal(fence.Get(),++fence_value));hr(fence->SetEventOnCompletion(fence_value,event));check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"Queued generations must complete");
    check(run(L"inflight-generation-restored")==baseline,"New generation must render neutral while old generation retains its coarse map");
    void* old_data{};D3D12_RANGE old_range{0,static_cast<SIZE_T>(size)},empty_range{};hr(old_pixels->Map(0,&old_range,&old_data));std::vector<unsigned char> old_image(1280*720*4);
    for(UINT y=0;y<720;++y)memcpy(old_image.data()+y*1280*4,static_cast<char*>(old_data)+y*footprint.Footprint.RowPitch,1280*4);old_pixels->Unmap(0,&empty_range);
    check(old_image!=baseline,"A later Reset must not modify the previous GPU generation's rate image");
    check(mode(disable)==0,"Final off");snapshot(nullptr);CloseHandle(event);hr(device->GetDeviceRemovedReason());
    if(cpu_mode){
        std::ofstream measurements(root/L"cpu-recording.json");measurements<<"{\"kind\":\"native_api_recording_only\",\"debug_layer\":"<<(debug?"true":"false")<<",\"iterations\":20000,\"runs\":[";
        LARGE_INTEGER timer_frequency{};QueryPerformanceFrequency(&timer_frequency);
        const bool order[]{false,true,true,false,false,true,true,false};
        for(unsigned sample=0;sample<std::size(order);++sample){
            wchar_t on[]=L"on",off[]=L"off";check(cpu_mode(order[sample]?on:off)==0,"CPU measurement mode");
            hr(allocator->Reset());hr(commands->Reset(allocator.Get(),pso.Get()));
            D3D12_VIEWPORT viewport{0,0,1280,720,0,1};D3D12_RECT scissor{0,0,1280,720};
            const float blend[]{1,1,1,1};
            LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
            for(unsigned repeat=0;repeat<20000;++repeat){
                commands->SetPipelineState(pso.Get());commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
                commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->OMSetBlendFactor(blend);commands->OMSetStencilRef(0);
            }
            QueryPerformanceCounter(&end);hr(commands->Close());
            if(sample)measurements<<',';measurements<<"{\"enabled\":"<<(order[sample]?"true":"false")<<",\"ms\":"<<double(end.QuadPart-begin.QuadPart)*1000.0/timer_frequency.QuadPart<<'}';
        }
        measurements<<"]}";wchar_t off[]=L"off";check(cpu_mode(off)==0,"CPU state rollback");snapshot(nullptr);
    }

    if(diagnostics){for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T size=0;diagnostics->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());hr(diagnostics->GetMessage(i,message,&size));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 validation error");}}}
    std::cout<<"Single-recording VRS: cached/passive/cross-queue rollback, late fallback, application-rate preservation and D3D12 validation PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
