#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <chrono>
#include <vector>
using Microsoft::WRL::ComPtr;
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Native HRESULT "+std::to_string(r));}
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string read(const std::filesystem::path& p){std::ifstream f(p);return {(std::istreambuf_iterator<char>(f)),{}};}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"DLL and new output directory required");const auto directory=std::filesystem::absolute(argv[2]);
    check(!std::filesystem::exists(directory),"Use fresh output");std::filesystem::create_directories(directory);
    HMODULE dll=LoadLibraryW(argv[1]);check(dll!=nullptr,"Load probe");using Api=DWORD(WINAPI*)(void*);
    const auto init=reinterpret_cast<Api>(GetProcAddress(dll,"ArcInitialize"));
    const auto start=reinterpret_cast<Api>(GetProcAddress(dll,"ArcBeginCapture"));const auto end=reinterpret_cast<Api>(GetProcAddress(dll,"ArcEndCapture"));const auto snapshot=reinterpret_cast<Api>(GetProcAddress(dll,"ArcSnapshot"));
    check(init&&start&&end&&snapshot,"Capture exports");auto metrics=(directory/L"metrics.json").wstring();check(init(metrics.data())==0,"Initialize observer");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    auto buffer=[&](D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4096;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;};
    auto source=buffer(D3D12_HEAP_TYPE_UPLOAD),middle=buffer(D3D12_HEAP_TYPE_DEFAULT),destination=buffer(D3D12_HEAP_TYPE_READBACK);
    void* data{};D3D12_RANGE empty{0,0};hr(source->Map(0,&empty,&data));for(UINT i=0;i<1024;++i)static_cast<UINT*>(data)[i]=i*37;source->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=1024;
    auto first=heap->GetCPUDescriptorHandleForHeapStart(),second=first;second.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);device->CreateShaderResourceView(middle.Get(),&srv,first);device->CopyDescriptorsSimple(1,second,first,hd.Type);
    check(start(nullptr)==0,"Begin capture");hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));
    list->CopyBufferRegion(middle.Get(),0,source.Get(),0,4096);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={middle.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    list->CopyBufferRegion(destination.Get(),0,middle.Get(),0,4096);hr(list->Close());ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");hr(queue->Signal(fence.Get(),1));hr(fence->SetEventOnCompletion(1,event));check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU wait");CloseHandle(event);
    hr(destination->Map(0,nullptr,&data));for(UINT i=0;i<1024;++i)check(static_cast<UINT*>(data)[i]==i*37,"Hook altered copied data");destination->Unmap(0,&empty);
    auto graph=(directory/L"graph.json").wstring();check(end(graph.data())==0,"End capture");check(snapshot(nullptr)==0,"Snapshot");
    auto json=read(metrics);check(json.find("\"resources_alive\":3")!=std::string::npos,"Observed resource count");check(json.find("\"descriptors_alive\":2")!=std::string::npos,"Descriptor copy observation");
    std::filesystem::copy_file(metrics,directory/L"alive.json");
    for(int i=0;i<1000;++i){auto temporary=buffer(D3D12_HEAP_TYPE_DEFAULT);}
    check(snapshot(nullptr)==0,"Churn snapshot");json=read(metrics);check(json.find("\"resources_alive\":3")!=std::string::npos,"Resource lifetime leaked during churn");
    std::filesystem::copy_file(metrics,directory/L"churn.json");
    {
        D3D12_DESCRIPTOR_HEAP_DESC large_desc=hd;large_desc.NumDescriptors=131072;ComPtr<ID3D12DescriptorHeap> large;
        hr(device->CreateDescriptorHeap(&large_desc,IID_PPV_ARGS(&large)));const auto begin=large->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(large_desc.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};null_view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;null_view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;null_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;null_view.Texture2D.MipLevels=1;
        for(UINT i=0;i<large_desc.NumDescriptors;++i){auto h=begin;h.ptr+=UINT64(i)*stride;device->CreateShaderResourceView(nullptr,&null_view,h);}
        snapshot(nullptr);auto state=read(metrics);
        check(state.find("\"null_descriptors\":131072")!=std::string::npos&&state.find("\"descriptors_alive\":2")!=std::string::npos,"Null initialization consumed sparse descriptor budget");
        std::filesystem::copy_file(metrics,directory/L"null-bitmap.json");
        device->CopyDescriptorsSimple(1,second,begin,hd.Type);snapshot(nullptr);state=read(metrics);
        check(state.find("\"descriptors_alive\":1")!=std::string::npos&&state.find("\"null_descriptors\":131073")!=std::string::npos,"Null copy did not invalidate previous resource view");
        device->CopyDescriptorsSimple(1,second,first,hd.Type);
    }
    {
        std::vector<ComPtr<ID3D12Resource>> live;for(int i=0;i<1500;++i)live.push_back(buffer(D3D12_HEAP_TYPE_DEFAULT));
        const auto t=std::chrono::steady_clock::now();
        for(int i=0;i<20000;++i)device->CopyDescriptorsSimple(1,second,first,hd.Type);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
        std::ofstream(directory/L"descriptor-cost.json")<<"{\"live_extra_resources\":1500,\"copies\":20000,\"cpu_ms\":"<<ms<<'}';
        std::cout<<"Descriptor observation cost (1500 live resources, 20000 copies): "<<ms<<" ms\n";
    }
    // Request a frame through the injected module; no resources, bindings or
    // semantic labels are passed to ARC by this application.
    const auto request=reinterpret_cast<Api>(GetProcAddress(dll,"ArcRequestFrame"));check(request!=nullptr,"Frame request export");
    ComPtr<IDXGIFactory2> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ARCGenericFrameFixture";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);check(window!=nullptr,"frame window");
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=64;sd.Height=64;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferCount=2;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&base));ComPtr<IDXGISwapChain3> swap;hr(base.As(&swap));
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.NumDescriptors=2;rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;ComPtr<ID3D12DescriptorHeap> rtvs;hr(device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&rtvs)));
    ComPtr<ID3D12Resource> backs[2];for(UINT i=0;i<2;++i){hr(swap->GetBuffer(i,IID_PPV_ARGS(&backs[i])));auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*device->GetDescriptorHandleIncrementSize(rh.Type);device->CreateRenderTargetView(backs[i].Get(),nullptr,h);}
    auto frame_path=(directory/L"frame.json").wstring();check(request(frame_path.data())==0,"Request actual frame");
    HANDLE frame_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(frame_event!=nullptr,"frame event");
    for(UINT frame=0;frame<4;++frame){
        hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));const auto index=swap->GetCurrentBackBufferIndex();
        barrier.Transition={backs[index].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};list->ResourceBarrier(1,&barrier);
        auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=index*device->GetDescriptorHandleIncrementSize(rh.Type);const FLOAT color[]{.2f,.4f,.6f,1};list->ClearRenderTargetView(h,color,0,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);hr(list->Close());queue->ExecuteCommandLists(1,commands);if(frame==1)queue->ExecuteCommandLists(1,commands);hr(queue->Signal(fence.Get(),frame+2));hr(fence->SetEventOnCompletion(frame+2,frame_event));check(WaitForSingleObject(frame_event,30000)==WAIT_OBJECT_0,"frame completion");hr(swap->Present(0,0));
    }
    CloseHandle(frame_event);
    for(int i=0;i<30&&!std::filesystem::exists(frame_path);++i)Sleep(100);
    check(std::filesystem::exists(frame_path),"Asynchronous frame export");
    check(read(frame_path).find("\"capture_state_complete\":true")!=std::string::npos,"Simple native frame lost observed application state");
    backs[0].Reset();backs[1].Reset();rtvs.Reset();swap.Reset();base.Reset();factory.Reset();DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    heap.Reset();source.Reset();middle.Reset();destination.Reset();list.Reset();queue.Reset();
    fence.Reset();
    check(snapshot(nullptr)==0,"Final snapshot");json=read(metrics);
    check(json.find("\"resources_alive\":0")!=std::string::npos&&json.find("\"descriptors_alive\":0")!=std::string::npos&&json.find("\"command_records\":0")!=std::string::npos,"Observer retained objects after native Release");
    std::filesystem::copy_file(metrics,directory/L"retired.json");
    std::cout<<"Generic native interception: 2 ordered copies, verified GPU data, descriptor copy, 1000 real resource lifetimes and retirement PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
