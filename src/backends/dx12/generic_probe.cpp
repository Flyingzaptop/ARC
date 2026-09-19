#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"
#include "arc/perceptual_trial.hpp"
#include "generic_runtime.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
namespace runtime=arc::dx12::generic;
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
bool observe_api() noexcept {return recording.load(std::memory_order_relaxed)&&!inside_present;}
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
    const bool outer=!inside_present;inside_present=true;const auto resource=outer&&recording&&!(flags&DXGI_PRESENT_TEST)?runtime::before_present(self):0;
    const HRESULT result=original_present(self,sync,flags);inside_present=!outer;
    if(outer){observe_present(self,sync,flags,result);if(recording)runtime::after_present(self,resource,result,flags);}return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* self,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters){
    const bool outer=!inside_present;inside_present=true;auto* base=reinterpret_cast<IDXGISwapChain*>(self);const auto resource=outer&&recording&&!(flags&DXGI_PRESENT_TEST)?runtime::before_present(base):0;
    const HRESULT result=original_present1(self,sync,flags,parameters);inside_present=!outer;
    if(outer){observe_present(base,sync,flags,result);if(recording)runtime::after_present(base,resource,result,flags);}return result;
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* commands){
    original_execute(self,count,commands);if(observe_api()){submits.fetch_add(1,std::memory_order_relaxed);lists.fetch_add(count,std::memory_order_relaxed);runtime::submit(self,count,commands);}
}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self,UINT vertices,UINT instances,UINT start,UINT instance){
    original_draw(self,vertices,instances,start,instance);if(observe_api()){draws.fetch_add(1,std::memory_order_relaxed);runtime::work(self,0,UINT64(vertices)*instances);}
}
void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self,UINT indices,UINT instances,UINT start,INT base,UINT instance){
    original_indexed(self,indices,instances,start,base,instance);if(observe_api()){indexed.fetch_add(1,std::memory_order_relaxed);runtime::work(self,1,UINT64(indices)*instances);}
}
void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* self,UINT x,UINT y,UINT z){
    original_dispatch(self,x,y,z);if(observe_api()){dispatches.fetch_add(1,std::memory_order_relaxed);runtime::work(self,2,UINT64(x)*y*z);}
}
HRESULT STDMETHODCALLTYPE resource(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out){
    const HRESULT result=original_resource(self,heap,flags,desc,state,clear,iid,out);
    if(SUCCEEDED(result)&&out&&*out&&observe_api()){
        resources.fetch_add(1,std::memory_order_relaxed);ID3D12Resource* r=nullptr;
        if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12Resource,reinterpret_cast<void**>(&r)))){runtime::observe_resource(r);ID3D12Resource_Release(r);}
    }return result;
}

using HeapFn=decltype(ID3D12DeviceVtbl::CreateDescriptorHeap);HeapFn original_heap{};
using SrvFn=decltype(ID3D12DeviceVtbl::CreateShaderResourceView);SrvFn original_srv{};
using UavFn=decltype(ID3D12DeviceVtbl::CreateUnorderedAccessView);UavFn original_uav{};
using RtvFn=decltype(ID3D12DeviceVtbl::CreateRenderTargetView);RtvFn original_rtv{};
using DsvFn=decltype(ID3D12DeviceVtbl::CreateDepthStencilView);DsvFn original_dsv{};
using SamplerFn=decltype(ID3D12DeviceVtbl::CreateSampler);SamplerFn original_sampler{};
using CbvFn=decltype(ID3D12DeviceVtbl::CreateConstantBufferView);CbvFn original_cbv{};
using DescriptorCopyFn=decltype(ID3D12DeviceVtbl::CopyDescriptorsSimple);DescriptorCopyFn original_descriptors{};
using DescriptorRangesFn=decltype(ID3D12DeviceVtbl::CopyDescriptors);DescriptorRangesFn original_descriptor_ranges{};
using FenceFn=decltype(ID3D12CommandQueueVtbl::Signal);FenceFn original_signal{},original_wait{};
using IndirectFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteIndirect);IndirectFn original_indirect{};
using BundleFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteBundle);BundleFn original_bundle{};
using BarriersFn=decltype(ID3D12GraphicsCommandListVtbl::ResourceBarrier);BarriersFn original_barriers{};
using ResetFn=decltype(ID3D12GraphicsCommandListVtbl::Reset);ResetFn original_reset{};
using CloseFn=decltype(ID3D12GraphicsCommandListVtbl::Close);CloseFn original_close{};
using PipelineFn=decltype(ID3D12GraphicsCommandListVtbl::SetPipelineState);PipelineFn original_pipeline{};
using TargetsFn=decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets);TargetsFn original_targets{};
using ViewportFn=decltype(ID3D12GraphicsCommandListVtbl::RSSetViewports);ViewportFn original_viewport{};
using ScissorFn=decltype(ID3D12GraphicsCommandListVtbl::RSSetScissorRects);ScissorFn original_scissor{};
using BindHeapsFn=decltype(ID3D12GraphicsCommandListVtbl::SetDescriptorHeaps);BindHeapsFn original_bind_heaps{};
using TableFn=decltype(ID3D12GraphicsCommandListVtbl::SetGraphicsRootDescriptorTable);TableFn original_graphics_table{},original_compute_table{};
using CopyFn=decltype(ID3D12GraphicsCommandListVtbl::CopyResource);CopyFn original_copy{};
using CopyBufferFn=decltype(ID3D12GraphicsCommandListVtbl::CopyBufferRegion);CopyBufferFn original_copy_buffer{};
using ClearFn=decltype(ID3D12GraphicsCommandListVtbl::ClearRenderTargetView);ClearFn original_clear{};
using RootFn=decltype(ID3D12GraphicsCommandListVtbl::SetGraphicsRootSignature);RootFn original_graphics_root{},original_compute_root{};
using SwapHwndFn=decltype(IDXGIFactory2Vtbl::CreateSwapChainForHwnd);SwapHwndFn original_swap_hwnd{};
using SwapFn=decltype(IDXGIFactoryVtbl::CreateSwapChain);SwapFn original_swap{};
HRESULT STDMETHODCALLTYPE swap_hwnd(IDXGIFactory2* f,IUnknown* d,HWND window,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* output,IDXGISwapChain1** out){const auto result=original_swap_hwnd(f,d,window,desc,full,output,out);if(observe_api()&&SUCCEEDED(result)&&out&&*out)runtime::observe_swapchain(reinterpret_cast<IDXGISwapChain*>(*out),d);return result;}
HRESULT STDMETHODCALLTYPE create_swap(IDXGIFactory* f,IUnknown* d,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out){const auto result=original_swap(f,d,desc,out);if(observe_api()&&SUCCEEDED(result)&&out&&*out)runtime::observe_swapchain(*out,d);return result;}
HRESULT STDMETHODCALLTYPE heap(ID3D12Device* d,const D3D12_DESCRIPTOR_HEAP_DESC* desc,REFIID iid,void** out){
    const auto result=original_heap(d,desc,iid,out);
    if(SUCCEEDED(result)&&out&&*out&&observe_api()){ID3D12DescriptorHeap* h=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12DescriptorHeap,reinterpret_cast<void**>(&h)))){runtime::observe_heap(h);ID3D12DescriptorHeap_Release(h);}}return result;
}
void STDMETHODCALLTYPE srv(ID3D12Device* d,ID3D12Resource* r,const D3D12_SHADER_RESOURCE_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_srv(d,r,v,h);if(observe_api())runtime::observe_view(r,h,1,v&&v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D?v->Texture2D.MostDetailedMip:0,v&&v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D?v->Texture2D.MipLevels:0);}
void STDMETHODCALLTYPE uav(ID3D12Device* d,ID3D12Resource* r,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_uav(d,r,counter,v,h);if(observe_api())runtime::observe_view(r,h,2,v&&v->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE rtv(ID3D12Device* d,ID3D12Resource* r,const D3D12_RENDER_TARGET_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_rtv(d,r,v,h);if(observe_api())runtime::observe_view(r,h,3,v&&v->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE dsv(ID3D12Device* d,ID3D12Resource* r,const D3D12_DEPTH_STENCIL_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_dsv(d,r,v,h);if(observe_api())runtime::observe_view(r,h,4,v&&v->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE sampler(ID3D12Device* d,const D3D12_SAMPLER_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_sampler(d,v,h);if(observe_api())runtime::observe_view(nullptr,h,5,0,0);}
void STDMETHODCALLTYPE cbv(ID3D12Device* d,const D3D12_CONSTANT_BUFFER_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){original_cbv(d,v,h);if(observe_api())runtime::observe_cbv(v,h);}
void STDMETHODCALLTYPE descriptors(ID3D12Device* d,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type){original_descriptors(d,count,dst,src,type);if(observe_api())runtime::copy_descriptors(count,dst,src,type,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));}
void STDMETHODCALLTYPE descriptor_ranges(ID3D12Device* d,UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* ds,UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type){original_descriptor_ranges(d,dc,dst,ds,sc,src,ss,type);if(observe_api())runtime::descriptor_ranges(dc,dst,ds,sc,src,ss,type,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));}
HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 value){const auto result=original_signal(q,f,value);if(observe_api()&&SUCCEEDED(result))runtime::fence(q,f,value,false);return result;}
HRESULT STDMETHODCALLTYPE wait(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 value){const auto result=original_wait(q,f,value);if(observe_api()&&SUCCEEDED(result))runtime::fence(q,f,value,true);return result;}
void STDMETHODCALLTYPE indirect(ID3D12GraphicsCommandList* c,ID3D12CommandSignature* signature,UINT count,ID3D12Resource* arguments,UINT64 offset,ID3D12Resource* counts,UINT64 count_offset){original_indirect(c,signature,count,arguments,offset,counts,count_offset);if(observe_api())runtime::unsupported();}
void STDMETHODCALLTYPE bundle(ID3D12GraphicsCommandList* c,ID3D12GraphicsCommandList* b){original_bundle(c,b);if(observe_api())runtime::unsupported();}
void STDMETHODCALLTYPE barriers(ID3D12GraphicsCommandList* c,UINT count,const D3D12_RESOURCE_BARRIER* b){original_barriers(c,count,b);if(observe_api())for(UINT i=0;i<count;++i)if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING)runtime::unsupported();}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* c,ID3D12CommandAllocator* a,ID3D12PipelineState* p){const auto result=original_reset(c,a,p);if(SUCCEEDED(result)&&observe_api()){runtime::begin(c);runtime::pipeline(c,p);}return result;}
HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList* c){const auto result=original_close(c);if(SUCCEEDED(result)&&observe_api())runtime::close(c);return result;}
void STDMETHODCALLTYPE pipeline(ID3D12GraphicsCommandList* c,ID3D12PipelineState* p){original_pipeline(c,p);if(observe_api())runtime::pipeline(c,p);}
void STDMETHODCALLTYPE targets(ID3D12GraphicsCommandList* c,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* h,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){original_targets(c,count,h,contiguous,depth);if(observe_api())runtime::targets(c,count,h,contiguous,depth);}
void STDMETHODCALLTYPE viewport(ID3D12GraphicsCommandList* c,UINT count,const D3D12_VIEWPORT* v){original_viewport(c,count,v);if(observe_api())runtime::viewport(c,count,v);}
void STDMETHODCALLTYPE scissor(ID3D12GraphicsCommandList* c,UINT count,const D3D12_RECT* v){original_scissor(c,count,v);if(observe_api())runtime::scissor(c,count,v);}
void STDMETHODCALLTYPE bind_heaps(ID3D12GraphicsCommandList* c,UINT count,ID3D12DescriptorHeap*const* h){original_bind_heaps(c,count,h);if(observe_api())runtime::descriptor_heaps(c,count,h);}
void STDMETHODCALLTYPE graphics_table(ID3D12GraphicsCommandList* c,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE h){original_graphics_table(c,parameter,h);if(observe_api())runtime::root_table(c,false,parameter,h);}
void STDMETHODCALLTYPE compute_table(ID3D12GraphicsCommandList* c,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE h){original_compute_table(c,parameter,h);if(observe_api())runtime::root_table(c,true,parameter,h);}
void STDMETHODCALLTYPE copy(ID3D12GraphicsCommandList* c,ID3D12Resource* dst,ID3D12Resource* src){original_copy(c,dst,src);if(observe_api())runtime::copy(c,src,dst,0,true);}
void STDMETHODCALLTYPE copy_buffer(ID3D12GraphicsCommandList* c,ID3D12Resource* dst,UINT64 dst_offset,ID3D12Resource* src,UINT64 src_offset,UINT64 bytes){original_copy_buffer(c,dst,dst_offset,src,src_offset,bytes);if(observe_api())runtime::copy(c,src,dst,bytes,false);}
void STDMETHODCALLTYPE clear(ID3D12GraphicsCommandList* c,D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT color[4],UINT count,const D3D12_RECT* rects){original_clear(c,h,color,count,rects);if(observe_api())runtime::clear_target(c,h,count==0);}
void STDMETHODCALLTYPE graphics_root(ID3D12GraphicsCommandList* c,ID3D12RootSignature* root){original_graphics_root(c,root);if(observe_api())runtime::root_signature(c,false,root);}
void STDMETHODCALLTYPE compute_root(ID3D12GraphicsCommandList* c,ID3D12RootSignature* root){original_compute_root(c,root);if(observe_api())runtime::root_signature(c,true,root);}
template<class T>bool install(T target,T replacement,T* original){
    // Trampolines outlive the bootstrap device; keep the owning runtime module
    // loaded for the process lifetime, just like the probe itself.
    HMODULE owner{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(target),&owner)){++hook_failures;return false;}
    const auto status=MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(original));
    if(status!=MH_OK){++hook_failures;return false;}return true;
}
void snapshot(){
    static std::mutex snapshot_mutex;std::lock_guard snapshot_lock(snapshot_mutex);
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
        <<",\"runtime\":";runtime::snapshot(file);
    file<<",\"coverage_complete\":false,\"note\":\"Object/descriptor lifetime tracking and bounded submitted-work capture. Shader accesses are possible candidates; no generic mutation or replay reference is available.\"}\n";
    file.close();MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING);
}
DWORD WINAPI logger(void*){
    for(;;){try{runtime::flush_capture();snapshot();}catch(...){}Sleep(1000);}return 0;
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
        ok=install(factory->lpVtbl->CreateSwapChainForHwnd,swap_hwnd,&original_swap_hwnd)&&ok;
        ok=install(reinterpret_cast<SwapFn>(factory->lpVtbl->CreateSwapChain),create_swap,&original_swap)&&ok;
        ok=install(device->lpVtbl->CreateDescriptorHeap,heap,&original_heap)&&ok;
        ok=install(device->lpVtbl->CreateShaderResourceView,srv,&original_srv)&&ok;
        ok=install(device->lpVtbl->CreateUnorderedAccessView,uav,&original_uav)&&ok;
        ok=install(device->lpVtbl->CreateRenderTargetView,rtv,&original_rtv)&&ok;
        ok=install(device->lpVtbl->CreateDepthStencilView,dsv,&original_dsv)&&ok;
        ok=install(device->lpVtbl->CreateSampler,sampler,&original_sampler)&&ok;
        ok=install(device->lpVtbl->CreateConstantBufferView,cbv,&original_cbv)&&ok;
        ok=install(device->lpVtbl->CopyDescriptorsSimple,descriptors,&original_descriptors)&&ok;
        ok=install(device->lpVtbl->CopyDescriptors,descriptor_ranges,&original_descriptor_ranges)&&ok;
        ok=install(queue->lpVtbl->Signal,signal,&original_signal)&&ok;
        ok=install(queue->lpVtbl->Wait,wait,&original_wait)&&ok;
        ok=install(command->lpVtbl->ExecuteIndirect,indirect,&original_indirect)&&ok;
        ok=install(command->lpVtbl->ExecuteBundle,bundle,&original_bundle)&&ok;
        ok=install(command->lpVtbl->ResourceBarrier,barriers,&original_barriers)&&ok;
        ok=install(command->lpVtbl->Reset,reset,&original_reset)&&ok;
        ok=install(command->lpVtbl->Close,close,&original_close)&&ok;
        ok=install(command->lpVtbl->SetPipelineState,pipeline,&original_pipeline)&&ok;
        ok=install(command->lpVtbl->OMSetRenderTargets,targets,&original_targets)&&ok;
        ok=install(command->lpVtbl->RSSetViewports,viewport,&original_viewport)&&ok;
        ok=install(command->lpVtbl->RSSetScissorRects,scissor,&original_scissor)&&ok;
        ok=install(command->lpVtbl->SetDescriptorHeaps,bind_heaps,&original_bind_heaps)&&ok;
        ok=install(command->lpVtbl->SetGraphicsRootDescriptorTable,graphics_table,&original_graphics_table)&&ok;
        ok=install(command->lpVtbl->SetComputeRootDescriptorTable,compute_table,&original_compute_table)&&ok;
        ok=install(command->lpVtbl->SetGraphicsRootSignature,graphics_root,&original_graphics_root)&&ok;
        ok=install(command->lpVtbl->SetComputeRootSignature,compute_root,&original_compute_root)&&ok;
        ok=install(command->lpVtbl->CopyResource,copy,&original_copy)&&ok;
        ok=install(command->lpVtbl->CopyBufferRegion,copy_buffer,&original_copy_buffer)&&ok;
        ok=install(command->lpVtbl->ClearRenderTargetView,clear,&original_clear)&&ok;
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
extern "C" __declspec(dllexport) DWORD WINAPI ArcBeginCapture(void*){runtime::begin_capture();return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcEndCapture(void* path){if(!path)return 1;try{runtime::end_capture(static_cast<const wchar_t*>(path));return 0;}catch(...){return 2;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcSnapshot(void*){try{snapshot();return 0;}catch(...){return 1;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestFrame(void* path){if(!path)return 1;try{return runtime::request_frame(static_cast<const wchar_t*>(path))?0:2;}catch(...){return 3;}}
