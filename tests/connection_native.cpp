#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void hr(HRESULT value){check(SUCCEEDED(value),"DX12 call failed");}
int wmain(int argc,wchar_t** argv)try{
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
    const bool before=GetModuleHandleW(L"arc-dx12-probe.dll")!=nullptr;
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_ROOT_SIGNATURE_DESC root_desc{};ComPtr<ID3DBlob> root_blob;hr(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));
    ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char source[]="[numthreads(1,1,1)] void main() {}";ComPtr<ID3DBlob> shader;hr(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,nullptr));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};pso_desc.pRootSignature=root.Get();pso_desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;hr(device->CreateComputePipelineState(&pso_desc,IID_PPV_ARGS(&pso)));
    D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
    ComPtr<IDXGIFactory2> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ArcConnectionFixture";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);check(window,"hidden window");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=2;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap));
    if(argc==4){const auto dll=LoadLibraryW(argv[3]);check(dll,"late load");const auto initialize=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll,"ArcInitialize"));auto file=(output/L"arc.json").wstring();check(initialize&&initialize(file.data())==0,"late initialize");}
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    for(unsigned i=1;i<=100;++i){hr(allocator->Reset());hr(list->Reset(allocator.Get(),pso.Get()));list->SetComputeRootSignature(root.Get());list->Dispatch(1,1,1);hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),i));hr(fence->SetEventOnCompletion(i,event));check(WaitForSingleObject(event,2000)==WAIT_OBJECT_0,"GPU completion");hr(swap->Present(0,0));Sleep(25);}
    const auto dll=GetModuleHandleW(L"arc-dx12-probe.dll");check(dll,"ARC loaded");const auto snapshot=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll,"ArcSnapshot"));check(snapshot&&snapshot(nullptr)==0,"snapshot");
    std::ofstream(output/L"renderer.json")<<"{\"pid\":"<<GetCurrentProcessId()<<",\"arc_before_main\":"<<(before?"true":"false")<<",\"device_healthy\":"<<(device->GetDeviceRemovedReason()==S_OK?"true":"false")<<"}";
    CloseHandle(event);DestroyWindow(window);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
