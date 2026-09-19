// Ordinary DX12 application. No arc-core linkage or cooperative resource labels.
// Optional DLL exports only select the same experimental mode as the launcher.
#include "dynamic_city_shader.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using Clock=std::chrono::steady_clock;
double millis(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
void hr(HRESULT result){if(FAILED(result))throw std::runtime_error("DX12 HRESULT "+std::to_string(result));}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
constexpr UINT W=1920,H=1080,ShadowSize=2048,Slots=3,Queries=5,MaxInstances=4096;
constexpr auto ReadState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Vertex {XMFLOAT3 p,n;XMFLOAT2 uv;};
struct Instance {XMFLOAT4X4 world;XMFLOAT4 tint,material;};
static_assert(sizeof(Instance)==96);
struct alignas(256) FrameData {XMFLOAT4X4 vp,inv,light;XMFLOAT4 camera,extent;XMFLOAT4 positions[24],colors[24];};
struct Object {XMFLOAT3 p,scale;XMFLOAT4 tint,material;float yaw{};bool shadow{true};int movement{};};
struct Sample {UINT tick{},sequence{};double cpu{},record{},submit{},present{},wait{},timing_readback{},gpu{},shadow{},geometry{},fog{},composite{};XMFLOAT3 camera{};UINT64 gpu_begin{},gpu_end{};};
float hash(UINT n){n=(n^61)^(n>>16);n*=9;n^=n>>4;n*=0x27d4eb2d;n^=n>>15;return float(n&65535)/65535.f;}
void save_png(const std::filesystem::path& path,const unsigned char* data,UINT pitch){
    ComPtr<IWICImagingFactory> factory;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;hr(factory->CreateStream(&stream));hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;hr(encoder->CreateNewFrame(&frame,&properties));hr(frame->Initialize(properties.Get()));hr(frame->SetSize(W,H));
    auto format=GUID_WICPixelFormat24bppBGR;hr(frame->SetPixelFormat(&format));
    ComPtr<IWICBitmap> bitmap;hr(factory->CreateBitmapFromMemory(W,H,GUID_WICPixelFormat32bppRGBA,pitch,pitch*H,const_cast<BYTE*>(data),&bitmap));
    ComPtr<IWICFormatConverter> converter;hr(factory->CreateFormatConverter(&converter));hr(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    hr(frame->WriteSource(converter.Get(),nullptr));hr(frame->Commit());hr(encoder->Commit());
}
class City {
    struct Slot {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> constants,instances,timing;void* cb{};void* ib{};UINT64 fence{};bool pending{};Sample sample;};
    ComPtr<IDXGIFactory6> factory;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12Fence> fence;HANDLE event{};UINT64 serial{},frequency{};HWND window{};bool tearing{},present_enabled{true};
    ComPtr<ID3D12DescriptorHeap> rtv,dsv,views;UINT rtv_stride{},dsv_stride{},view_stride{};
    std::array<ComPtr<ID3D12Resource>,Slots> backs;
    ComPtr<ID3D12Resource> vertices,indices,materials,shadow,depth,scene,fog,readback;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};D3D12_INDEX_BUFFER_VIEW index_view{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> shadow_pso,geometry_pso,fog_pso,composite_pso;
    ComPtr<ID3D12QueryHeap> queries;std::array<Slot,Slots> slots;
    std::vector<Object> objects;std::vector<Instance> instances;UINT casters{};std::filesystem::path output;
    std::vector<Sample> samples;UINT frame_number{},pose_count{900};std::string adapter_name;UINT64 dedicated_vram{};
    D3D12_CPU_DESCRIPTOR_HANDLE rt(UINT i){auto h=rtv->GetCPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*rtv_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE ds(UINT i){auto h=dsv->GetCPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*dsv_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE view(UINT i){auto h=views->GetCPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*view_stride;return h;}
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;}
    ComPtr<ID3D12Resource> texture(UINT width,UINT height,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,const D3D12_CLEAR_VALUE* clear=nullptr){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=flags;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,ReadState,clear,IID_PPV_ARGS(&r)));return r;}
    void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&barrier);}
    void wait(UINT64 value){if(value&&fence->GetCompletedValue()<value){hr(fence->SetEventOnCompletion(value,event));require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU fence timeout");}}
    void collect(Slot& slot){if(!slot.pending)return;wait(slot.fence);void* data{};D3D12_RANGE range{0,Queries*sizeof(UINT64)},empty{0,0};hr(slot.timing->Map(0,&range,&data));const auto* t=static_cast<UINT64*>(data);
        auto ms=[&](int a,int b){require(t[b]>=t[a],"Nonmonotonic GPU timestamps");return double(t[b]-t[a])*1000.0/double(frequency);};
        slot.sample.gpu_begin=t[0];slot.sample.gpu_end=t[4];slot.sample.gpu=ms(0,4);slot.sample.shadow=ms(0,1);slot.sample.geometry=ms(1,2);slot.sample.fog=ms(2,3);slot.sample.composite=ms(3,4);slot.timing->Unmap(0,&empty);samples.push_back(slot.sample);slot.pending=false;}
    ComPtr<ID3DBlob> compile(const char* entry,const char* profile){ComPtr<ID3DBlob> code,error;const auto result=D3DCompile(dynamic_city_shader,sizeof(dynamic_city_shader)-1,"dynamic-city",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);if(FAILED(result)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());hr(result);return code;}
    void add(XMFLOAT3 p,XMFLOAT3 scale,XMFLOAT4 tint,XMFLOAT4 material,bool cast=true,int movement=0,float yaw=0){objects.push_back({p,scale,tint,material,yaw,cast,movement});}
    void build_city(){
        add({0,-.25f,90},{15,.5f,240},{.5f,.55f,.65f,0},{0,1,.6f,.22f});
        for(int side:{-1,1}){add({side*9.f,0,90},{3,.3f,240},{.8f,.8f,.85f,0},{1,.7f,0,.7f});
            for(UINT row=0;row<21;++row){const UINT seed=row*2+(side>0);const float z=float(row)*11-18,h=9+hash(seed+10)*22,x=side*(14+hash(seed+3)*2);
                add({x,h*.5f,z},{8,h,9},{.45f+hash(seed)*.25f,.48f,.58f,0},{1,1,0,.65f});
                for(UINT floor=0;floor<UINT(h/3);++floor)for(int col=-1;col<=1;++col){const float lit=hash(seed*71+floor*11+col+29)>.3f?1.f:0.f;
                    const bool cyan=hash(seed+floor)>.5f;add({x-side*4.025f,1.7f+floor*3.f,z+col*2.5f},{.035f,1.3f,1.4f},{cyan?.2f:1.f,cyan?.7f:.2f,cyan?1.f:.4f,lit*1.5f},{2,1,.2f,.4f},false);}
                const bool cyan=seed%2;add({x-side*4.08f,3.5f,z},{.08f,1.4f,4.5f},{cyan?.1f:1.f,cyan?.6f:.05f,cyan?1.f:.35f,4},{3,1,0,.5f},false);
                add({side*6.9f,1.9f,z},{.14f,3.8f,.14f},{.22f,.25f,.3f,0},{2,1,.7f,.3f});
                add({side*6.9f,3.9f,z},{.55f,.12f,.55f},{.5f,.65f,1,4},{2,1,0,.3f},false);
            }}
        for(int i=0;i<48;++i)add({0,.015f,float(i)*4-18},{.1f,.025f,1.6f},{.8f,.62f,.25f,0},{2,1,0,.8f},false);
        for(int car=0;car<18;++car){const float x=car%2?2.5f:-2.5f,z=car*11.f-20;const int motion=car%2?1:-1;const float yaw=car%2?0:XM_PI;
            add({x,.6f,z},{1.65f,.75f,3.3f},{.12f+hash(car)*.45f,.13f+hash(car+3)*.35f,.17f+hash(car+7)*.4f,0},{2,1,.75f,.17f},true,motion,yaw);
            add({x,1.13f,z-.12f},{1.25f,.5f,1.6f},{.035f,.065f,.095f,0},{2,1,.8f,.12f},true,motion,yaw);
            for(int sign:{-1,1}){add({x+sign*.57f,.7f,z+motion*1.67f},{.4f,.16f,.07f},{1,.9f,.65f,6},{2,1,0,.5f},false,motion);
                for(int end:{-1,1})add({x+sign*.85f,.35f,z+end*1.f},{.24f,.5f,.65f},{.04f,.04f,.04f,0},{0,1,0,.9f},true,motion);}}
        for(int i=0;i<36;++i){const float x=(i%2?1:-1)*(8.4f+hash(i+66));add({x,.85f,i*5.f-12},{.45f,1.5f,.4f},{.1f+hash(i)*.3f,.1f,.2f+hash(i+4)*.25f,0},{1,1,0,.9f},true,i%2?2:-2);
            add({x,1.75f,i*5.f-12},{.35f,.35f,.35f},{.4f,.26f,.2f,0},{2,1,0,.85f},true,i%2?2:-2);}
        for(UINT i=0;i<70;++i)add({(i%2?1.f:-1.f)*(9+hash(i+87)*2),.35f,float(i)*3-18},{.5f+hash(i),.7f,.65f},{.12f,.2f,.23f,0},{2,1,.3f,.7f});
        std::stable_sort(objects.begin(),objects.end(),[](const auto& a,const auto& b){return a.shadow>b.shadow;});casters=static_cast<UINT>(std::count_if(objects.begin(),objects.end(),[](const auto& o){return o.shadow;}));
        require(objects.size()<=MaxInstances,"Scene exceeds instance budget");instances.resize(objects.size());
    }
    FrameData update(UINT tick){const float t=float(tick)*15.f/pose_count;const float phase=float(tick)*XM_2PI/pose_count-XM_PIDIV2;
        FrameData data{};const XMFLOAT3 camera{4*std::sin(phase),2.5f+std::sin(phase*2)*.25f,75+65*std::cos(phase)};
        const auto eye=XMLoadFloat3(&camera);const auto forward=XMVector3Normalize(XMVectorSet(4*std::cos(phase),0,-65*std::sin(phase),0));
        const auto target=eye+forward*8+XMVectorSet(0,.15f,0,0);
        const auto vp=XMMatrixLookAtLH(eye,target,XMVectorSet(0,1,0,0))*XMMatrixPerspectiveFovLH(XMConvertToRadians(68),float(W)/H,.1f,180);
        XMStoreFloat4x4(&data.vp,vp);XMStoreFloat4x4(&data.inv,XMMatrixInverse(nullptr,vp));
        const auto light=XMMatrixLookAtLH(XMVectorSet(-30,48,camera.z-10,1),XMVectorSet(0,0,camera.z+20,1),XMVectorSet(0,1,0,0))*XMMatrixOrthographicLH(95,100,.1f,170);
        XMStoreFloat4x4(&data.light,light);data.camera={camera.x,camera.y,camera.z,t};data.extent={float(W),float(H),1.f/W,1.f/H};
        for(UINT i=0;i<24;++i){const float z=std::floor((camera.z-20)/11)*11+float(i/2)*11;const bool cyan=i%2;
            data.positions[i]={(i%2?1.f:-1.f)*6.5f,3.5f,z,12};const float pulse=.9f+.1f*std::sin(t*.7f+i);
            data.colors[i]={pulse*(cyan?.2f:2.5f),pulse*(cyan?1.5f:.2f),pulse*(cyan?3.f:.65f),1};}
        for(UINT i=0;i<objects.size();++i){const auto& o=objects[i];auto p=o.p;
            if(std::abs(o.movement)==1)p.z=std::fmod(p.z+25+o.movement*t*(220.f/15)+2200,220)-25;
            if(std::abs(o.movement)==2){p.z+=3*std::sin(phase*2+i);p.x+=.15f*std::sin(phase*8+i);}
            XMStoreFloat4x4(&instances[i].world,XMMatrixScaling(o.scale.x,o.scale.y,o.scale.z)*XMMatrixRotationY(o.yaw)*XMMatrixTranslation(p.x,p.y,p.z));instances[i].tint=o.tint;instances[i].material=o.material;
        }return data;
    }
public:
    explicit City(std::filesystem::path path,UINT poses,bool present):output(std::move(path)),pose_count(poses),present_enabled(present){
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;const auto result=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));if(result==DXGI_ERROR_NOT_FOUND)break;hr(result);DXGI_ADAPTER_DESC1 desc{};hr(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))){for(const auto c:desc.Description){if(!c)break;adapter_name+=c<128?char(c):'?';}dedicated_vram=desc.DedicatedVideoMemory;break;}}
        require(device!=nullptr,"Hardware DX12 device required");D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));hr(queue->GetTimestampFrequency(&frequency));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Fence event");
        BOOL allow=FALSE;ComPtr<IDXGIFactory5> f5;if(SUCCEEDED(factory.As(&f5))&&SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow))))tearing=allow!=FALSE;
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ARCDynamicCity";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"ARC dynamic city — deterministic DX12 benchmark",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1280,760,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"Create scene window");
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=W;sd.Height=H;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferCount=Slots;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Flags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
        ComPtr<IDXGISwapChain1> base;hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&base));hr(base.As(&swap));factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
        auto make_heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,D3D12_DESCRIPTOR_HEAP_FLAGS flags,ComPtr<ID3D12DescriptorHeap>& out){D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=type;d.NumDescriptors=count;d.Flags=flags;hr(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&out)));};
        make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,Slots+1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,rtv);make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,dsv);make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,views);
        rtv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);dsv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);view_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(UINT i=0;i<Slots;++i){
            if(present_enabled)hr(swap->GetBuffer(i,IID_PPV_ARGS(&backs[i])));
            else{D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC offscreen{};offscreen.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;offscreen.Width=W;offscreen.Height=H;offscreen.DepthOrArraySize=offscreen.MipLevels=1;offscreen.Format=sd.Format;offscreen.SampleDesc.Count=1;offscreen.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&offscreen,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&backs[i])));}
            device->CreateRenderTargetView(backs[i].Get(),nullptr,rt(i));}
        D3D12_CLEAR_VALUE depth_clear{};depth_clear.Format=DXGI_FORMAT_D32_FLOAT;depth_clear.DepthStencil.Depth=1;
        shadow=texture(ShadowSize,ShadowSize,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,&depth_clear);depth=texture(W,H,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,&depth_clear);
        scene=texture(W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);fog=texture((W+1)/2,(H+1)/2,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;device->CreateDepthStencilView(shadow.Get(),&dv,ds(0));device->CreateDepthStencilView(depth.Get(),&dv,ds(1));device->CreateRenderTargetView(scene.Get(),nullptr,rt(Slots));
        auto make_srv=[&](ID3D12Resource* resource,DXGI_FORMAT format,UINT index){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(resource,&d,view(index));};
        make_srv(shadow.Get(),DXGI_FORMAT_R32_FLOAT,1);make_srv(scene.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,2);make_srv(depth.Get(),DXGI_FORMAT_R32_FLOAT,3);make_srv(fog.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(fog.Get(),nullptr,&uv,view(5));
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,5}};
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor.ShaderRegister=0;parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={2,ranges};
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};for(UINT i=0;i<3;++i){auto& s=samplers[i];s.ShaderRegister=i;s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=i?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:D3D12_TEXTURE_ADDRESS_MODE_WRAP;s.MaxLOD=D3D12_FLOAT32_MAX;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;}
        samplers[1].Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;samplers[1].ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=3;rd.pStaticSamplers=samplers;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob,errors;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_INPUT_ELEMENT_DESC layout[]{{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"WORLD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"WORLD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"WORLD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,1,32,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"WORLD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,1,48,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,64,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,1,80,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1}};
        auto vs=compile("geometry_vs","vs_5_0"),ps=compile("geometry_ps","ps_5_0"),sv=compile("shadow_vs","vs_5_0"),fv=compile("fullscreen_vs","vs_5_0"),fp=compile("composite_ps","ps_5_0"),cs=compile("fog_cs","cs_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};pd.InputLayout={layout,UINT(std::size(layout))};pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&geometry_pso)));pd.VS={sv->GetBufferPointer(),sv->GetBufferSize()};pd.PS={};pd.NumRenderTargets=0;pd.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;pd.RasterizerState.DepthBias=700;pd.RasterizerState.SlopeScaledDepthBias=1.5f;hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&shadow_pso)));
        pd.VS={fv->GetBufferPointer(),fv->GetBufferSize()};pd.PS={fp->GetBufferPointer(),fp->GetBufferSize()};pd.InputLayout={};pd.DepthStencilState.DepthEnable=FALSE;pd.DSVFormat=DXGI_FORMAT_UNKNOWN;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.RasterizerState.DepthBias=0;pd.RasterizerState.SlopeScaledDepthBias=0;hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&composite_pso)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};cd.pRootSignature=root.Get();cd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};hr(device->CreateComputePipelineState(&cd,IID_PPV_ARGS(&fog_pso)));
        D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=Slots*Queries;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));
        for(auto& s:slots){hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)));hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list)));hr(s.list->Close());s.constants=buffer(sizeof(FrameData),D3D12_HEAP_TYPE_UPLOAD);s.instances=buffer(MaxInstances*sizeof(Instance),D3D12_HEAP_TYPE_UPLOAD);s.timing=buffer(Queries*8,D3D12_HEAP_TYPE_READBACK);D3D12_RANGE empty{0,0};hr(s.constants->Map(0,&empty,&s.cb));hr(s.instances->Map(0,&empty,&s.ib));}
        build_city();build_assets();ShowWindow(window,SW_SHOW);UpdateWindow(window);
    }
    void build_assets(){
        std::vector<Vertex> verts;std::vector<UINT16> inds;
        auto face=[&](XMFLOAT3 n,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d){const auto base=UINT16(verts.size());verts.insert(verts.end(),{{a,n,{0,1}},{b,n,{1,1}},{c,n,{1,0}},{d,n,{0,0}}});for(UINT16 i:{0,1,2,0,2,3})inds.push_back(base+i);};
        face({0,0,-1},{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f});face({0,0,1},{.5f,-.5f,.5f},{-.5f,-.5f,.5f},{-.5f,.5f,.5f},{.5f,.5f,.5f});
        face({-1,0,0},{-.5f,-.5f,.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,.5f,.5f});face({1,0,0},{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f});
        face({0,1,0},{-.5f,.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f});face({0,-1,0},{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,-.5f,-.5f},{-.5f,-.5f,-.5f});
        vertices=buffer(verts.size()*sizeof(Vertex),D3D12_HEAP_TYPE_UPLOAD);indices=buffer(inds.size()*2,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE empty{0,0};hr(vertices->Map(0,&empty,&mapped));memcpy(mapped,verts.data(),verts.size()*sizeof(Vertex));vertices->Unmap(0,nullptr);hr(indices->Map(0,&empty,&mapped));memcpy(mapped,inds.data(),inds.size()*2);indices->Unmap(0,nullptr);
        vertex_view={vertices->GetGPUVirtualAddress(),UINT(verts.size()*sizeof(Vertex)),sizeof(Vertex)};index_view={indices->GetGPUVirtualAddress(),UINT(inds.size()*2),DXGI_FORMAT_R16_UINT};
        constexpr UINT side=512,mips=10,layers=4;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=side;td.Height=side;td.DepthOrArraySize=layers;td.MipLevels=mips;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&materials)));
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,mips*layers> footprints;UINT64 total{};device->GetCopyableFootprints(&td,0,mips*layers,0,footprints.data(),nullptr,nullptr,&total);auto upload=buffer(total,D3D12_HEAP_TYPE_UPLOAD);hr(upload->Map(0,&empty,&mapped));
        for(UINT layer=0;layer<layers;++layer){std::vector<unsigned char> mip(side*side*4);for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const auto i=(y*side+x)*4;const int noise=int(hash(x+y*side+layer*13)*20)-10;
                int base=layer==0?40:layer==1?95:150;if(layer==1&&((y%32)<2||((x+(y/32%2)*32)%64)<2))base=50;
                if(layer==2&&((x%128)<3||(y%128)<3))base=90;if(layer==3)base=((x/16+y/24)%5==0||x%64<6)?240:35;
                for(int c=0;c<3;++c)mip[i+c]=static_cast<unsigned char>(std::clamp(base+noise+(c==2?9:0),0,255));mip[i+3]=255;}
            UINT dim=side;for(UINT level=0;level<mips;++level){const auto& f=footprints[layer*mips+level];for(UINT y=0;y<dim;++y)memcpy(static_cast<unsigned char*>(mapped)+f.Offset+UINT64(y)*f.Footprint.RowPitch,mip.data()+y*dim*4,dim*4);
                if(dim>1){std::vector<unsigned char> next((dim/2)*(dim/2)*4);for(UINT y=0;y<dim/2;++y)for(UINT x=0;x<dim/2;++x)for(UINT c=0;c<4;++c){UINT value=0;for(UINT dy=0;dy<2;++dy)for(UINT dx=0;dx<2;++dx)value+=mip[((y*2+dy)*dim+x*2+dx)*4+c];next[(y*(dim/2)+x)*4+c]=static_cast<unsigned char>(value/4);}mip=std::move(next);dim/=2;}}
        }upload->Unmap(0,nullptr);auto& s=slots[0];hr(s.allocator->Reset());hr(s.list->Reset(s.allocator.Get(),nullptr));
        for(UINT i=0;i<mips*layers;++i){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=footprints[i];b.pResource=materials.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.SubresourceIndex=i;s.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);}
        transition(s.list.Get(),materials.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState);hr(s.list->Close());ID3D12CommandList* list[]{s.list.Get()};queue->ExecuteCommandLists(1,list);hr(queue->Signal(fence.Get(),++serial));wait(serial);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=mips;srv.Texture2DArray.ArraySize=layers;device->CreateShaderResourceView(materials.Get(),&srv,view(0));
        const auto back_desc=backs[0]->GetDesc();device->GetCopyableFootprints(&back_desc,0,1,0,&footprint,nullptr,nullptr,&total);readback=buffer(total,D3D12_HEAP_TYPE_READBACK);
    }
    void render(UINT tick,bool measured,const std::filesystem::path& png={}){
        const auto start=Clock::now();const UINT index=frame_number++%Slots;auto& s=slots[index];const auto wait_start=Clock::now();wait(s.fence);const double waited=millis(Clock::now()-wait_start);const auto timing_start=Clock::now();collect(s);const double timing_cpu=millis(Clock::now()-timing_start);
        const auto record_start=Clock::now();const auto data=update(tick);memcpy(s.cb,&data,sizeof(data));memcpy(s.ib,instances.data(),instances.size()*sizeof(Instance));
        hr(s.allocator->Reset());hr(s.list->Reset(s.allocator.Get(),shadow_pso.Get()));auto* list=s.list.Get();const UINT query=index*Queries;
        auto stamp=[&](UINT q){list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+q);};stamp(0);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootSignature(root.Get());list->SetGraphicsRootConstantBufferView(0,s.constants->GetGPUVirtualAddress());list->SetGraphicsRootDescriptorTable(1,views->GetGPUDescriptorHandleForHeapStart());
        D3D12_VERTEX_BUFFER_VIEW vb[]{vertex_view,{s.instances->GetGPUVirtualAddress(),UINT(instances.size()*sizeof(Instance)),sizeof(Instance)}};list->IASetVertexBuffers(0,2,vb);list->IASetIndexBuffer(&index_view);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        transition(list,shadow.Get(),ReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);auto shadow_dsv=ds(0);list->OMSetRenderTargets(0,nullptr,FALSE,&shadow_dsv);list->ClearDepthStencilView(shadow_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        D3D12_VIEWPORT viewport{0,0,float(ShadowSize),float(ShadowSize),0,1};D3D12_RECT scissor{0,0,ShadowSize,ShadowSize};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->DrawIndexedInstanced(36,casters,0,0,0);transition(list,shadow.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,ReadState);stamp(1);
        transition(list,scene.Get(),ReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);transition(list,depth.Get(),ReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);auto hdr_rtv=rt(Slots),depth_dsv=ds(1);list->OMSetRenderTargets(1,&hdr_rtv,FALSE,&depth_dsv);const float sky[]{.008f,.012f,.029f,1};list->ClearRenderTargetView(hdr_rtv,sky,0,nullptr);list->ClearDepthStencilView(depth_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        viewport={0,0,float(W),float(H),0,1};scissor={0,0,W,H};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->SetPipelineState(geometry_pso.Get());list->DrawIndexedInstanced(36,UINT(instances.size()),0,0,0);transition(list,scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,ReadState);transition(list,depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,ReadState);stamp(2);
        transition(list,fog.Get(),ReadState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->SetPipelineState(fog_pso.Get());list->SetComputeRootSignature(root.Get());list->SetComputeRootConstantBufferView(0,s.constants->GetGPUVirtualAddress());list->SetComputeRootDescriptorTable(1,views->GetGPUDescriptorHandleForHeapStart());list->Dispatch(((W+1)/2+7)/8,((H+1)/2+7)/8,1);transition(list,fog.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ReadState);stamp(3);
        const auto back=present_enabled?swap->GetCurrentBackBufferIndex():index;transition(list,backs[back].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);auto back_rtv=rt(back);list->OMSetRenderTargets(1,&back_rtv,FALSE,nullptr);list->SetPipelineState(composite_pso.Get());list->DrawInstanced(3,1,0,0);stamp(4);
        if(!png.empty()){transition(list,backs[back].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=backs[back].Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);transition(list,backs[back].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);}
        else transition(list,backs[back].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query,Queries,s.timing.Get(),0);hr(list->Close());const double record_ms=millis(Clock::now()-record_start);
        const auto submit_start=Clock::now();ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);const double submit_ms=millis(Clock::now()-submit_start);
        const auto present_start=Clock::now();if(present_enabled)hr(swap->Present(0,tearing?DXGI_PRESENT_ALLOW_TEARING:0));const double present_ms=millis(Clock::now()-present_start);hr(queue->Signal(fence.Get(),s.fence=++serial));
        s.sample={};s.sample.tick=tick;s.sample.sequence=frame_number-1;s.sample.cpu=millis(Clock::now()-start);s.sample.record=record_ms;s.sample.submit=submit_ms;s.sample.present=present_ms;s.sample.wait=waited;s.sample.timing_readback=timing_cpu;s.sample.camera={data.camera.x,data.camera.y,data.camera.z};s.pending=measured;
        if(!png.empty()){wait(s.fence);void* mapped{};D3D12_RANGE range{0,SIZE_T(footprint.Footprint.RowPitch)*H},empty{0,0};hr(readback->Map(0,&range,&mapped));save_png(png,static_cast<const unsigned char*>(mapped),footprint.Footprint.RowPitch);readback->Unmap(0,&empty);}
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)throw std::runtime_error("Scene window closed");TranslateMessage(&message);DispatchMessageW(&message);}
    }
    void drain(){wait(serial);for(auto& s:slots)collect(s);hr(device->GetDeviceRemovedReason());}
    void benchmark(UINT poses,bool optimized,double minimum_seconds){
        UINT warmup=0;const auto warm_start=Clock::now();do{render(warmup++%poses,false);}while(millis(Clock::now()-warm_start)<1500);drain();samples.clear();samples.reserve(65536);const auto start=Clock::now();
        UINT frames=0;do{for(UINT tick=0;tick<poses;++tick){render(tick,true);++frames;if(millis(Clock::now()-start)>50000)throw std::runtime_error("50-second measurement budget exhausted; incomplete run");}}while(millis(Clock::now()-start)<minimum_seconds*1000);
        drain();const double seconds=millis(Clock::now()-start)/1000.;require(samples.size()==frames,"Missing frame measurements");std::sort(samples.begin(),samples.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
        std::ofstream csv(output/L"frames.csv");csv<<"frame,tick,camera_x,camera_y,camera_z,cpu_frame_ms,record_ms,submit_ms,present_ms,fence_wait_ms,timing_readback_ms,gpu_ms,shadow_ms,geometry_ms,fog_ms,composite_ms,gpu_begin_tick,gpu_end_tick\n";csv<<std::setprecision(10);
        for(const auto& s:samples)csv<<s.sequence<<','<<s.tick<<','<<s.camera.x<<','<<s.camera.y<<','<<s.camera.z<<','<<s.cpu<<','<<s.record<<','<<s.submit<<','<<s.present<<','<<s.wait<<','<<s.timing_readback<<','<<s.gpu<<','<<s.shadow<<','<<s.geometry<<','<<s.fog<<','<<s.composite<<','<<s.gpu_begin<<','<<s.gpu_end<<'\n';
        std::ofstream meta(output/L"run.json");meta<<std::setprecision(12)<<"{\"scene\":\"dynamic-city-v1\",\"adapter\":"<<std::quoted(adapter_name)<<",\"dedicated_vram_bytes\":"<<dedicated_vram<<",\"gpu_timestamp_frequency\":"<<frequency<<",\"width\":"<<W<<",\"height\":"<<H<<",\"instances\":"<<objects.size()<<",\"shadow_casters\":"<<casters<<",\"triangles_per_mesh\":12,\"lights\":24,\"fog_steps\":12,\"shadow_size\":2048,\"poses_per_cycle\":"<<poses<<",\"scene_cycle_seconds\":15,\"warmup_frames\":"<<warmup<<",\"measured_frames\":"<<frames<<",\"seconds\":"<<seconds<<",\"fps\":"<<frames/seconds<<",\"vsync\":false,\"tearing_supported\":"<<(tearing?"true":"false")<<",\"frames_in_flight\":3,\"arc_loaded\":"<<(GetModuleHandleW(L"arc-dx12-probe.dll")?"true":"false")<<",\"experimental_vrs\":"<<(optimized?"true":"false")<<",\"image_copies_during_measurement\":0,\"frame_generation\":false,\"presentation_enabled\":"<<(present_enabled?"true":"false")<<"}";
        std::cout<<"Measured "<<frames<<" frames in "<<seconds<<" s: "<<frames/seconds<<" FPS\n";
    }
    void screenshots(UINT frames){for(UINT i=0;i<12;++i){const UINT tick=i*(frames-1)/11;render(tick,false,output/(L"frame-"+std::to_wstring(tick)+L".png"));}drain();}
    ~City(){if(queue&&fence){queue->Signal(fence.Get(),++serial);if(fence->GetCompletedValue()<serial){fence->SetEventOnCompletion(serial,event);WaitForSingleObject(event,10000);}}if(window)DestroyWindow(window);if(event)CloseHandle(event);}
};
int wmain(int argc,wchar_t** argv)try{
    require(argc>=3,"Usage: arc-dynamic-city <fresh output directory> <poses 120..1800> [--seconds 1..40] [--arc DLL]");
    const auto output=std::filesystem::absolute(argv[1]);require(!std::filesystem::exists(output),"Fresh output directory required");std::filesystem::create_directories(output);const UINT poses=std::stoul(argv[2]);require(poses>=120&&poses<=1800,"Pose count 120..1800");double seconds=15;bool present=true;std::filesystem::path arc_path;
    for(int i=3;i<argc;++i){const std::wstring arg=argv[i];if(arg==L"--seconds"&&i+1<argc)seconds=std::stod(argv[++i]);else if(arg==L"--no-present")present=false;else if(arg==L"--arc"&&i+1<argc)arc_path=std::filesystem::absolute(argv[++i]);else throw std::runtime_error("Unknown scene option");}
    require(seconds>=1&&seconds<=40,"Measurement seconds 1..40");hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
    using Api=DWORD(WINAPI*)(void*);Api mode=nullptr,snapshot=nullptr;HMODULE dll=nullptr;
    if(!arc_path.empty()){dll=LoadLibraryW(arc_path.c_str());require(dll!=nullptr,"Load v0.1");auto init=reinterpret_cast<Api>(GetProcAddress(dll,"ArcInitialize"));auto lean=reinterpret_cast<Api>(GetProcAddress(dll,"ArcUseLeanMode"));mode=reinterpret_cast<Api>(GetProcAddress(dll,"ArcExperimentalVrs"));snapshot=reinterpret_cast<Api>(GetProcAddress(dll,"ArcSnapshot"));auto telemetry=(output/L"arc.json").wstring();require(init&&lean&&mode&&snapshot&&init(telemetry.data())==0&&lean(nullptr)==0,"Initialize v0.1 in lean mode");}
    City city(output,poses,present);if(mode){wchar_t enable[]=L"2x2";require(mode(enable)==0,"Enable current v0.1 experiment");}
    city.benchmark(poses,mode!=nullptr,seconds);if(snapshot)snapshot(nullptr);city.screenshots(poses);if(mode){wchar_t off[]=L"off";mode(off);snapshot(nullptr);}return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
