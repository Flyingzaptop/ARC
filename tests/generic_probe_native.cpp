#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t** argv)try{
    require(argc==2,"probe DLL path required");
    ComPtr<ID3D12Device> device;require(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"device");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;
    require(SUCCEEDED(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue))),"queue");
    ComPtr<IDXGIFactory2> factory;require(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))),"factory");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ARCProbeForwardFixture";
    RegisterClassW(&wc);HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"window");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=2;desc.Height=2;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;require(SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap)),"swapchain");
    // Invalid sync interval is deliberate and must be forwarded, not repaired or hidden.
    const HRESULT before=swap->Present(5,0);require(FAILED(before),"invalid Present control did not fail");
    const auto path=std::filesystem::absolute("generic-probe-native.json").wstring();
    HMODULE probe=LoadLibraryW(argv[1]);require(probe!=nullptr,"load probe");
    const auto initialize=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(probe,"ArcInitialize"));
    require(initialize&&initialize(const_cast<wchar_t*>(path.c_str()))==0,"initialize probe");
    const HRESULT after=swap->Present(5,0);require(after==before,"hook changed the native HRESULT");
    require(device->GetDeviceRemovedReason()==S_OK,"control caused device loss");
    bool observed=false;
    for(int attempt=0;attempt<30&&!observed;++attempt){
        Sleep(100);std::ifstream file(path);std::string json((std::istreambuf_iterator<char>(file)),{});
        observed=json.find("\"present_failures\":1")!=std::string::npos&&json.find("\"last_present_sync\":5")!=std::string::npos&&
            json.find("\"last_present_hresult\":"+std::to_string(after))!=std::string::npos&&
            json.find("\"device_reason_available\":true")!=std::string::npos&&json.find("\"last_device_removed_reason\":0")!=std::string::npos;
    }
    require(observed,"failure context was not faithfully recorded");
    swap.Reset();DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    // The module intentionally remains loaded until process exit, as in a target game.
    std::cout<<"Generic DX12 hook preserved native invalid-call HRESULT and parameters; device healthy PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
