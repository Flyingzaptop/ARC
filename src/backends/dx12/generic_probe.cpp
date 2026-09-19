#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"
#include "arc/perceptual_trial.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
std::atomic<bool> initialized{},initializing{},recording{};
std::atomic<unsigned long long> presents{},present_failures{},submits{},lists{},draws{},indexed{},dispatches{},resources{},hook_failures{};
std::atomic<long> last_present_error{},last_device_reason{};
std::atomic<UINT> last_present_sync{},last_present_flags{};
std::atomic<bool> device_reason_available{};
std::filesystem::path output;
HMODULE module{};
using PresentFn=decltype(IDXGISwapChainVtbl::Present);
using Present1Fn=decltype(IDXGISwapChain1Vtbl::Present1);
using ExecuteFn=decltype(ID3D12CommandQueueVtbl::ExecuteCommandLists);
using DrawFn=decltype(ID3D12GraphicsCommandListVtbl::DrawInstanced);
using IndexedFn=decltype(ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced);
using DispatchFn=decltype(ID3D12GraphicsCommandListVtbl::Dispatch);
using ResourceFn=decltype(ID3D12DeviceVtbl::CreateCommittedResource);
PresentFn original_present{};Present1Fn original_present1{};ExecuteFn original_execute{};
DrawFn original_draw{};IndexedFn original_indexed{};DispatchFn original_dispatch{};ResourceFn original_resource{};
thread_local bool inside_present{};
void observe_present(IDXGISwapChain* self,UINT sync,UINT flags,HRESULT result){
    if(!recording.load(std::memory_order_relaxed)||(flags&DXGI_PRESENT_TEST))return;
    presents.fetch_add(1,std::memory_order_relaxed);
    if(FAILED(result)){
        present_failures.fetch_add(1,std::memory_order_relaxed);last_present_error=result;last_present_sync=sync;last_present_flags=flags;
        ID3D12Device* device=nullptr;
        const auto query=IDXGISwapChain_GetDevice(self,IID_ID3D12Device,reinterpret_cast<void**>(&device));
        device_reason_available=SUCCEEDED(query)&&device;
        if(device){last_device_reason=ID3D12Device_GetDeviceRemovedReason(device);ID3D12Device_Release(device);}
    }
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* self,UINT sync,UINT flags){
    const bool outer=!inside_present;inside_present=true;const HRESULT result=original_present(self,sync,flags);inside_present=!outer;
    if(outer)observe_present(self,sync,flags,result);return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* self,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters){
    const bool outer=!inside_present;inside_present=true;const HRESULT result=original_present1(self,sync,flags,parameters);inside_present=!outer;
    if(outer)observe_present(reinterpret_cast<IDXGISwapChain*>(self),sync,flags,result);return result;
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* commands){
    original_execute(self,count,commands);if(recording.load(std::memory_order_relaxed)){submits.fetch_add(1,std::memory_order_relaxed);lists.fetch_add(count,std::memory_order_relaxed);}
}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self,UINT vertices,UINT instances,UINT start,UINT instance){
    original_draw(self,vertices,instances,start,instance);if(recording.load(std::memory_order_relaxed))draws.fetch_add(1,std::memory_order_relaxed);
}
void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self,UINT indices,UINT instances,UINT start,INT base,UINT instance){
    original_indexed(self,indices,instances,start,base,instance);if(recording.load(std::memory_order_relaxed))indexed.fetch_add(1,std::memory_order_relaxed);
}
void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* self,UINT x,UINT y,UINT z){
    original_dispatch(self,x,y,z);if(recording.load(std::memory_order_relaxed))dispatches.fetch_add(1,std::memory_order_relaxed);
}
HRESULT STDMETHODCALLTYPE resource(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out){
    const HRESULT result=original_resource(self,heap,flags,desc,state,clear,iid,out);
    if(SUCCEEDED(result)&&out&&*out&&recording.load(std::memory_order_relaxed))resources.fetch_add(1,std::memory_order_relaxed);return result;
}
template<class T>bool install(T target,T replacement,T* original){
    // Trampolines outlive the bootstrap device; keep the owning runtime module
    // loaded for the process lifetime, just like the probe itself.
    HMODULE owner{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(target),&owner)){++hook_failures;return false;}
    const auto status=MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(original));
    if(status!=MH_OK){++hook_failures;return false;}return true;
}
void snapshot(){
    // No reference replay/readback or safe mutation capability has been established
    // for an unknown process. The portable policy must therefore abstain.
    arc::PerceptualTrialController policy;
    const auto proposal=policy.choose({});
    const auto temporary=output.wstring()+L".tmp";
    std::ofstream file(temporary,std::ios::trunc);
    file<<"{\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"initialized\":"<<(initialized.load()?"true":"false")
        <<",\"mode\":\"observe_only\",\"engine_specific_code\":false,\"telemetry_scope\":\"partial_api_call_counts\","
          "\"replay_reference_available\":false,\"safe_mutation_capability\":false,\"quality_mutations\":0,"
          "\"policy_abstained\":"<<(!proposal?"true":"false")<<",\"present_calls\":"<<presents.load()
        <<",\"present_failures\":"<<present_failures.load()<<",\"queue_submits\":"<<submits.load()
        <<",\"last_present_hresult\":"<<last_present_error.load()<<",\"last_present_sync\":"<<last_present_sync.load()<<",\"last_present_flags\":"<<last_present_flags.load()
        <<",\"device_reason_available\":"<<(device_reason_available.load()?"true":"false")<<",\"last_device_removed_reason\":"<<last_device_reason.load()
        <<",\"submitted_lists\":"<<lists.load()<<",\"draw_calls\":"<<draws.load()
        <<",\"indexed_draw_calls\":"<<indexed.load()<<",\"dispatch_calls\":"<<dispatches.load()
        <<",\"committed_resources\":"<<resources.load()<<",\"hook_failures\":"<<hook_failures.load()
        <<",\"coverage_complete\":false,\"note\":\"Counts exclude bootstrap objects. Indirect/mesh work, newer allocation APIs and alternate implementations may be unobserved. No frame pixels or process memory are captured.\"}\n";
    file.close();MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING);
}
DWORD WINAPI logger(void*){
    for(;;){try{snapshot();}catch(...){}Sleep(1000);}return 0;
}
}

extern "C" __declspec(dllexport) DWORD WINAPI ArcInitialize(void* path){
    if(initialized.load())return 0;
    bool expected=false;if(!initializing.compare_exchange_strong(expected,true))return 7;
    if(!path)return 1;
    try{output=static_cast<const wchar_t*>(path);if(!output.is_absolute())return 2;std::filesystem::create_directories(output.parent_path());}catch(...){return 3;}
    if(MH_Initialize()!=MH_OK)return 4;
    ID3D12Device* device=nullptr;ID3D12CommandQueue* queue=nullptr;ID3D12CommandAllocator* allocator=nullptr;ID3D12GraphicsCommandList* command=nullptr;IDXGIFactory2* factory=nullptr;IDXGISwapChain1* swapchain=nullptr;
    HWND window=nullptr;DWORD result=5;
    do {
        if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_ID3D12Device,reinterpret_cast<void**>(&device))))break;
        D3D12_COMMAND_QUEUE_DESC q{};if(FAILED(ID3D12Device_CreateCommandQueue(device,&q,IID_ID3D12CommandQueue,reinterpret_cast<void**>(&queue))))break;
        if(FAILED(ID3D12Device_CreateCommandAllocator(device,D3D12_COMMAND_LIST_TYPE_DIRECT,IID_ID3D12CommandAllocator,reinterpret_cast<void**>(&allocator))))break;
        if(FAILED(ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&command))))break;
        if(FAILED(CreateDXGIFactory2(0,IID_IDXGIFactory2,reinterpret_cast<void**>(&factory))))break;
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=module;wc.lpszClassName=L"ARCReadOnlyBootstrap";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,2,2,nullptr,nullptr,module,nullptr);if(!window)break;
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=2;sd.Height=2;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if(FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory,reinterpret_cast<IUnknown*>(queue),window,&sd,nullptr,nullptr,&swapchain)))break;
        bool ok=true;
        ok=install(reinterpret_cast<PresentFn>(swapchain->lpVtbl->Present),present,&original_present)&&ok;
        ok=install(swapchain->lpVtbl->Present1,present1,&original_present1)&&ok;
        ok=install(queue->lpVtbl->ExecuteCommandLists,execute,&original_execute)&&ok;
        ok=install(command->lpVtbl->DrawInstanced,draw,&original_draw)&&ok;
        ok=install(command->lpVtbl->DrawIndexedInstanced,draw_indexed,&original_indexed)&&ok;
        ok=install(command->lpVtbl->Dispatch,dispatch,&original_dispatch)&&ok;
        ok=install(device->lpVtbl->CreateCommittedResource,resource,&original_resource)&&ok;
        if(!ok)break;
        if(MH_EnableHook(MH_ALL_HOOKS)!=MH_OK)break;
        result=0;
    }while(false);
    if(swapchain)IDXGISwapChain1_Release(swapchain);if(factory)IDXGIFactory2_Release(factory);
    if(command)ID3D12GraphicsCommandList_Release(command);if(allocator)ID3D12CommandAllocator_Release(allocator);
    if(queue)ID3D12CommandQueue_Release(queue);if(device)ID3D12Device_Release(device);
    if(window)DestroyWindow(window);UnregisterClassW(L"ARCReadOnlyBootstrap",module);
    if(result){MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();try{snapshot();}catch(...){}return result;}
    initialized=true;recording=true;HANDLE thread=CreateThread(nullptr,0,logger,nullptr,0,nullptr);
    if(!thread){recording=false;return 6;}CloseHandle(thread);return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){module=instance;DisableThreadLibraryCalls(instance);}return TRUE;
}
