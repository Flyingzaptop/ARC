#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"
#include "arc/perceptual_trial.hpp"
#include "arc/intercept_cpu_meter.hpp"
#include "generic_runtime.hpp"
#include "generic_command_mirror.hpp"
#include "generic_pixel_optimizer.hpp"
#include "generic_gpu_profile.hpp"
#include "generic_optimizer.hpp"
#include "generic_hook_control.hpp"
#include "generic_child_launch.hpp"
#include "generic_auto_session.hpp"
#include "generic_cpu_workers.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <array>
#include <set>
#include <tuple>

namespace {
namespace runtime=arc::dx12::generic;
namespace mirror=arc::dx12::mirror;
namespace pixel=arc::dx12::pixel;
namespace profile=arc::dx12::gpu_profile;
namespace optimizer=arc::dx12::optimizer;
namespace autotune=arc::dx12::autotune;
std::atomic<bool> mirror_hooks_ready{};
std::atomic<bool> detailed_tracking{true};
std::atomic<bool> passive_hooks{};
std::set<void*> detailed_only_targets;
std::set<void*> optimizer_targets;
std::set<void*> raster_targets;
std::atomic<unsigned> raster_setup{};
std::atomic<bool> raster_hooks_enabled{true};
std::array<void*,256> installed_targets{};std::size_t installed_count{};
std::mutex hook_mode_mutex;
std::atomic<bool> initialized{},initializing{},recording{};
std::atomic<unsigned long long> presents{},present_failures{},submits{},lists{},draws{},indexed{},dispatches{},resources{},hook_failures{};
std::atomic<long> last_present_error{},last_device_reason{};
std::atomic<UINT> last_present_sync{},last_present_flags{};
std::atomic<bool> device_reason_available{};
std::filesystem::path output;
std::mutex output_mutex;
HMODULE module{};
using PresentFn=decltype(IDXGISwapChainVtbl::Present);
using Present1Fn=decltype(IDXGISwapChain1Vtbl::Present1);
using ColorSpaceFn=decltype(IDXGISwapChain3Vtbl::SetColorSpace1);ColorSpaceFn original_color_space{};
HRESULT STDMETHODCALLTYPE color_space(IDXGISwapChain3* swap,DXGI_COLOR_SPACE_TYPE value){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=arc::original_cpu_call([&]{return original_color_space(swap,value);});if(recording&&SUCCEEDED(result))runtime::observe_color_space(reinterpret_cast<IDXGISwapChain*>(swap),value);return result;}
using ExecuteFn=decltype(ID3D12CommandQueueVtbl::ExecuteCommandLists);
using DrawFn=decltype(ID3D12GraphicsCommandListVtbl::DrawInstanced);
using IndexedFn=decltype(ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced);
using DispatchFn=decltype(ID3D12GraphicsCommandListVtbl::Dispatch);
using ResourceFn=decltype(ID3D12DeviceVtbl::CreateCommittedResource);
PresentFn original_present{};Present1Fn original_present1{};ExecuteFn original_execute{};
DrawFn original_draw{};IndexedFn original_indexed{};DispatchFn original_dispatch{};ResourceFn original_resource{};
thread_local bool inside_present{};
bool observe_api() noexcept {return recording.load(std::memory_order_relaxed)&&!inside_present&&!mirror::internal();}
void observe_present(IDXGISwapChain* self,UINT sync,UINT flags,HRESULT result){
    if(!recording.load(std::memory_order_relaxed)||(flags&DXGI_PRESENT_TEST))return;
    presents.fetch_add(1,std::memory_order_relaxed);if(result==S_OK)arc::dx12::placement::present(self);
    if(FAILED(result)){
        present_failures.fetch_add(1,std::memory_order_relaxed);last_present_error=result;last_present_sync=sync;last_present_flags=flags;
        ID3D12Device* device=nullptr;
        const auto query=IDXGISwapChain_GetDevice(self,IID_ID3D12Device,reinterpret_cast<void**>(&device));
        device_reason_available=SUCCEEDED(query)&&device;
        if(device){last_device_reason=ID3D12Device_GetDeviceRemovedReason(device);ID3D12Device_Release(device);}
    }
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* self,UINT sync,UINT flags){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const bool outer=!inside_present;inside_present=true;const auto resource=outer&&recording&&!(flags&DXGI_PRESENT_TEST)?runtime::before_present(self):0;
    const HRESULT result=arc::original_cpu_call([&]{return original_present(self,sync,flags);});inside_present=!outer;
    if(outer){if(result==S_OK&&!(flags&DXGI_PRESENT_TEST))profile::present(self);observe_present(self,sync,flags,result);if(recording)runtime::after_present(self,resource,result,flags);autotune::present(self,result,flags);}return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* self,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const bool outer=!inside_present;inside_present=true;auto* base=reinterpret_cast<IDXGISwapChain*>(self);const auto resource=outer&&recording&&!(flags&DXGI_PRESENT_TEST)?runtime::before_present(base):0;
    const HRESULT result=arc::original_cpu_call([&]{return original_present1(self,sync,flags,parameters);});inside_present=!outer;
    if(outer){if(result==S_OK&&!(flags&DXGI_PRESENT_TEST))profile::present(base);observe_present(base,sync,flags,result);if(recording)runtime::after_present(base,resource,result,flags);autotune::present(base,result,flags);}return result;
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* commands){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const bool observed=observe_api();auto pixel_ticket=observed?pixel::before_submit(self,count,commands):pixel::Submission{};auto ticket=observed?profile::before_submit(self,count,commands):profile::Submission{};
    if(!observed||(!optimizer::execute(self,count,commands)&&!mirror::execute(self,count,commands))){mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_execute(self,count,commands);});}
    if(observed){pixel::after_submit(pixel_ticket,self);profile::after_submit(ticket,self);submits.fetch_add(1,std::memory_order_relaxed);lists.fetch_add(count,std::memory_order_relaxed);if(detailed_tracking.load(std::memory_order_relaxed))runtime::submit(self,count,commands);}
}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self,UINT vertices,UINT instances,UINT start,UINT instance){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())profile::work(self,false,vertices,instances,1);
    auto lease=observe_api()?mirror::acquire_draw(self):mirror::Lease{};const bool changed=mirror::before_draw(lease);const bool pixel_changed=observe_api()&&pixel::before_draw(self);
    {mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_draw(self,vertices,instances,start,instance);});}if(pixel_changed)pixel::after_draw(self);if(changed)mirror::after_draw(lease);if(observe_api()){draws.fetch_add(1,std::memory_order_relaxed);if(detailed_tracking.load(std::memory_order_relaxed))runtime::work(self,0,UINT64(vertices)*instances);}
}
void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self,UINT indices,UINT instances,UINT start,INT base,UINT instance){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())profile::work(self,false,indices,instances,1);
    auto lease=observe_api()?mirror::acquire_draw(self):mirror::Lease{};const bool changed=mirror::before_draw(lease);const bool pixel_changed=observe_api()&&pixel::before_draw(self);
    {mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_indexed(self,indices,instances,start,base,instance);});}if(pixel_changed)pixel::after_draw(self);if(changed)mirror::after_draw(lease);if(observe_api()){indexed.fetch_add(1,std::memory_order_relaxed);if(detailed_tracking.load(std::memory_order_relaxed))runtime::work(self,1,UINT64(indices)*instances);}
}
void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* self,UINT x,UINT y,UINT z){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const bool observed=observe_api();if(observed)profile::work(self,true,x,y,z);if(!observed||!optimizer::dispatch(self,x,y,z)){mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_dispatch(self,x,y,z);});}if(observed)mirror::record(original_dispatch,self,x,y,z);if(observed){dispatches.fetch_add(1,std::memory_order_relaxed);if(detailed_tracking.load(std::memory_order_relaxed))runtime::work(self,2,UINT64(x)*y*z);}
}
HRESULT STDMETHODCALLTYPE resource(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const HRESULT result=arc::original_cpu_call([&]{return original_resource(self,heap,flags,desc,state,clear,iid,out);});
    if(SUCCEEDED(result)&&out&&*out&&observe_api()){
        resources.fetch_add(1,std::memory_order_relaxed);ID3D12Resource* r=nullptr;
        if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12Resource,reinterpret_cast<void**>(&r)))){optimizer::resource_created(r);if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_resource(r);ID3D12Resource_Release(r);}
    }return result;
}

using HeapFn=decltype(ID3D12DeviceVtbl::CreateDescriptorHeap);HeapFn original_heap{};
using RootCreateFn=decltype(ID3D12DeviceVtbl::CreateRootSignature);RootCreateFn original_root_create{};
using PlacedFn=decltype(ID3D12DeviceVtbl::CreatePlacedResource);PlacedFn original_placed{};
using Resource1Fn=decltype(ID3D12Device4Vtbl::CreateCommittedResource1);Resource1Fn original_resource1{};
HRESULT STDMETHODCALLTYPE resource1(ID3D12Device4* device,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protected_session,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=arc::original_cpu_call([&]{return original_resource1(device,heap,flags,desc,state,clear,protected_session,iid,out);});
    if(observe_api()&&optimizer::enabled()&&!protected_session&&SUCCEEDED(result)&&out&&*out){ID3D12Resource* resource=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12Resource,reinterpret_cast<void**>(&resource)))){optimizer::resource_created(resource);ID3D12Resource_Release(resource);}}return result;
}
HRESULT STDMETHODCALLTYPE root_create(ID3D12Device* d,UINT node,const void* data,SIZE_T size,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=arc::original_cpu_call([&]{return original_root_create(d,node,data,size,iid,out);});
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12RootSignature* root=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12RootSignature,reinterpret_cast<void**>(&root)))){pixel::root_created(root,data,size);mirror::root_created(root,data,size);optimizer::root_created(root,data,size);ID3D12RootSignature_Release(root);}}return result;
}
HRESULT STDMETHODCALLTYPE placed(ID3D12Device* d,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=arc::original_cpu_call([&]{return original_placed(d,heap,offset,desc,state,clear,iid,out);});
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12Resource* resource=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12Resource,reinterpret_cast<void**>(&resource)))){optimizer::resource_created(resource,heap,offset);ID3D12Resource_Release(resource);}}return result;
}
using SrvFn=decltype(ID3D12DeviceVtbl::CreateShaderResourceView);SrvFn original_srv{};
using UavFn=decltype(ID3D12DeviceVtbl::CreateUnorderedAccessView);UavFn original_uav{};
using RtvFn=decltype(ID3D12DeviceVtbl::CreateRenderTargetView);RtvFn original_rtv{};
using DsvFn=decltype(ID3D12DeviceVtbl::CreateDepthStencilView);DsvFn original_dsv{};
using SamplerFn=decltype(ID3D12DeviceVtbl::CreateSampler);SamplerFn original_sampler{};
using CbvFn=decltype(ID3D12DeviceVtbl::CreateConstantBufferView);CbvFn original_cbv{};
using DescriptorCopyFn=decltype(ID3D12DeviceVtbl::CopyDescriptorsSimple);DescriptorCopyFn original_descriptors{};
using FeedbackViewFn=decltype(ID3D12Device8Vtbl::CreateSamplerFeedbackUnorderedAccessView);FeedbackViewFn original_feedback_view{};
void STDMETHODCALLTYPE feedback_view(ID3D12Device8* device,ID3D12Resource* target,ID3D12Resource* feedback,D3D12_CPU_DESCRIPTOR_HANDLE destination){
    static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    auto guard=observe_api()?optimizer::descriptor_write():optimizer::DescriptorWrite{};
    arc::original_cpu_call([&]{return original_feedback_view(device,target,feedback,destination);});
    if(observe_api())optimizer::forget_descriptor(destination);
}
using DescriptorRangesFn=decltype(ID3D12DeviceVtbl::CopyDescriptors);DescriptorRangesFn original_descriptor_ranges{};
using FenceFn=decltype(ID3D12CommandQueueVtbl::Signal);FenceFn original_signal{},original_wait{};
using IndirectFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteIndirect);IndirectFn original_indirect{};
using BundleFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteBundle);BundleFn original_bundle{};
using BarriersFn=decltype(ID3D12GraphicsCommandListVtbl::ResourceBarrier);BarriersFn original_barriers{};
using ResetFn=decltype(ID3D12GraphicsCommandListVtbl::Reset);ResetFn original_reset{};
using CreateCommandFn=decltype(ID3D12DeviceVtbl::CreateCommandList);CreateCommandFn original_create_command{};
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
using ResizeFn=decltype(IDXGISwapChainVtbl::ResizeBuffers);ResizeFn original_resize{};
using Resize1Fn=decltype(IDXGISwapChain3Vtbl::ResizeBuffers1);Resize1Fn original_resize1{};
using FullscreenFn=decltype(IDXGISwapChainVtbl::SetFullscreenState);FullscreenFn original_fullscreen{};
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags){if(observe_api()){autotune::surface_changed(swap);if(!runtime::retire_images_before_resize())return DXGI_ERROR_WAS_STILL_DRAWING;}mirror::InternalCall internal;return original_resize(swap,count,width,height,format,flags);}
HRESULT STDMETHODCALLTYPE resize1(IDXGISwapChain3* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* nodes,IUnknown*const* queues){if(observe_api()){autotune::surface_changed(swap);if(!runtime::retire_images_before_resize())return DXGI_ERROR_WAS_STILL_DRAWING;}mirror::InternalCall internal;return original_resize1(swap,count,width,height,format,flags,nodes,queues);}
HRESULT STDMETHODCALLTYPE fullscreen(IDXGISwapChain* swap,BOOL enabled,IDXGIOutput* output){if(observe_api()){autotune::surface_changed(swap);if(!runtime::retire_images_before_resize())return DXGI_ERROR_WAS_STILL_DRAWING;}mirror::InternalCall internal;return original_fullscreen(swap,enabled,output);}
HRESULT STDMETHODCALLTYPE swap_hwnd(IDXGIFactory2* f,IUnknown* d,HWND window,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* output,IDXGISwapChain1** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=[&]{mirror::InternalCall internal;return arc::original_cpu_call([&]{return original_swap_hwnd(f,d,window,desc,full,output,out);});}();if(observe_api()&&SUCCEEDED(result)&&out&&*out)runtime::observe_swapchain(reinterpret_cast<IDXGISwapChain*>(*out),d);return result;}
HRESULT STDMETHODCALLTYPE create_swap(IDXGIFactory* f,IUnknown* d,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=[&]{mirror::InternalCall internal;return arc::original_cpu_call([&]{return original_swap(f,d,desc,out);});}();if(observe_api()&&SUCCEEDED(result)&&out&&*out)runtime::observe_swapchain(*out,d);return result;}
HRESULT STDMETHODCALLTYPE heap(ID3D12Device* d,const D3D12_DESCRIPTOR_HEAP_DESC* desc,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=arc::original_cpu_call([&]{return original_heap(d,desc,iid,out);});
    if(SUCCEEDED(result)&&out&&*out&&observe_api()){ID3D12DescriptorHeap* h=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12DescriptorHeap,reinterpret_cast<void**>(&h)))){optimizer::heap_created(h);if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_heap(h);ID3D12DescriptorHeap_Release(h);}}return result;
}
void STDMETHODCALLTYPE srv(ID3D12Device* d,ID3D12Resource* r,const D3D12_SHADER_RESOURCE_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};if(observed&&optimizer::cpu_same_view(1,r,nullptr,v,sizeof(*v),h))return;arc::original_cpu_call([&]{return original_srv(d,r,v,h);});if(observed)optimizer::srv(r,v,h);if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_view(r,h,1,v&&v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D?v->Texture2D.MostDetailedMip:0,v&&v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D?v->Texture2D.MipLevels:0);}
void STDMETHODCALLTYPE uav(ID3D12Device* d,ID3D12Resource* r,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};if(observed&&optimizer::cpu_same_view(2,r,counter,v,sizeof(*v),h))return;arc::original_cpu_call([&]{return original_uav(d,r,counter,v,h);});if(observed)optimizer::uav(r,counter,v,h);if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_view(r,h,2,v&&v->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE rtv(ID3D12Device* d,ID3D12Resource* r,const D3D12_RENDER_TARGET_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_rtv(d,r,v,h);});if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_view(r,h,3,v&&v->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE dsv(ID3D12Device* d,ID3D12Resource* r,const D3D12_DEPTH_STENCIL_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_dsv(d,r,v,h);});if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_view(r,h,4,v&&v->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D?v->Texture2D.MipSlice:0,1);}
void STDMETHODCALLTYPE sampler(ID3D12Device* d,const D3D12_SAMPLER_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};arc::original_cpu_call([&]{return original_sampler(d,v,h);});if(observed)optimizer::sampler(v,h);if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_view(nullptr,h,5,0,0);}
void STDMETHODCALLTYPE cbv(ID3D12Device* d,const D3D12_CONSTANT_BUFFER_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};if(observed&&optimizer::cpu_same_view(6,nullptr,nullptr,v,sizeof(*v),h))return;arc::original_cpu_call([&]{return original_cbv(d,v,h);});if(observed)optimizer::cbv(v,h);if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::observe_cbv(v,h);}
void STDMETHODCALLTYPE descriptors(ID3D12Device* d,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};arc::original_cpu_call([&]{return original_descriptors(d,count,dst,src,type);});if(observed)optimizer::copy_descriptors(count,dst,src,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::copy_descriptors(count,dst,src,type,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));}
void STDMETHODCALLTYPE descriptor_ranges(ID3D12Device* d,UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* ds,UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observed=observe_api();auto write_guard=observed?optimizer::descriptor_write():optimizer::DescriptorWrite{};arc::original_cpu_call([&]{return original_descriptor_ranges(d,dc,dst,ds,sc,src,ss,type);});if(observed)optimizer::descriptor_ranges(dc,dst,ds,sc,src,ss,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));if(observed)if(detailed_tracking.load(std::memory_order_relaxed))runtime::descriptor_ranges(dc,dst,ds,sc,src,ss,type,ID3D12Device_GetDescriptorHandleIncrementSize(d,type));}
HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 value){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=arc::original_cpu_call([&]{return original_signal(q,f,value);});if(observe_api()&&SUCCEEDED(result))if(detailed_tracking.load(std::memory_order_relaxed))runtime::fence(q,f,value,false);return result;}
HRESULT STDMETHODCALLTYPE wait(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 value){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=arc::original_cpu_call([&]{return original_wait(q,f,value);});if(observe_api()&&SUCCEEDED(result))if(detailed_tracking.load(std::memory_order_relaxed))runtime::fence(q,f,value,true);return result;}
void STDMETHODCALLTYPE indirect(ID3D12GraphicsCommandList* c,ID3D12CommandSignature* signature,UINT count,ID3D12Resource* arguments,UINT64 offset,ID3D12Resource* counts,UINT64 count_offset){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())optimizer::cpu_invalidate(c);
    if(observe_api())profile::indirect(c,signature,count);
    mirror::Lease lease;
    if(observe_api()&&mirror::requested_rate()){
        if(mirror::raster_indirect(signature))lease=mirror::acquire_draw(c);else mirror::invalidate(c);
    }
    const bool changed=mirror::before_draw(lease);if(observe_api())pixel::invalidate(c);
    {mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_indirect(c,signature,count,arguments,offset,counts,count_offset);});}if(changed)mirror::after_draw(lease);if(observe_api())optimizer::after_indirect(c,signature);
    if(observe_api()&&detailed_tracking)runtime::unsupported();
}

void STDMETHODCALLTYPE bundle(ID3D12GraphicsCommandList* c,ID3D12GraphicsCommandList* b){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);if(observe_api()){pixel::invalidate(c);mirror::invalidate(c);profile::opaque(c);optimizer::state_unknown(c);}arc::original_cpu_call([&]{return original_bundle(c,b);});if(observe_api()){if(detailed_tracking.load(std::memory_order_relaxed))runtime::unsupported();}}
void STDMETHODCALLTYPE barriers(ID3D12GraphicsCommandList* c,UINT count,const D3D12_RESOURCE_BARRIER* b){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_barriers(c,count,b);});if(observe_api())mirror::record(original_barriers,c,count,b);if(observe_api())for(UINT i=0;i<count;++i)if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING)if(detailed_tracking.load(std::memory_order_relaxed))runtime::unsupported();}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* c,ID3D12CommandAllocator* a,ID3D12PipelineState* p){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_reset(c,a,p);});}();if(SUCCEEDED(result)&&observe_api()){pixel::begin(c,p);mirror::begin(c,p);profile::begin(c,p);optimizer::begin(c,p);if(detailed_tracking.load(std::memory_order_relaxed))runtime::begin(c);if(detailed_tracking.load(std::memory_order_relaxed))runtime::pipeline(c,p);}return result;}
HRESULT STDMETHODCALLTYPE create_command(ID3D12Device* d,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* a,ID3D12PipelineState* p,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_create_command(d,node,type,a,p,iid,out);});}();
    if(SUCCEEDED(result)&&observe_api()&&out&&*out){ID3D12GraphicsCommandList* c=nullptr;
        if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&c)))){
            pixel::begin(c,p);mirror::begin(c,p);profile::begin(c,p);optimizer::begin(c,p);if(detailed_tracking.load(std::memory_order_relaxed))runtime::begin(c);if(detailed_tracking.load(std::memory_order_relaxed))runtime::pipeline(c,p);ID3D12GraphicsCommandList_Release(c);
        }}return result;
}
HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList* c){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);if(observe_api()){pixel::close(c);mirror::close(c);profile::close(c);optimizer::close(c);}const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_close(c);});}();if(SUCCEEDED(result)&&observe_api()){if(detailed_tracking.load(std::memory_order_relaxed))runtime::close(c);}return result;}
void STDMETHODCALLTYPE pipeline(ID3D12GraphicsCommandList* c,ID3D12PipelineState* p){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observing=observe_api();if(observing&&p&&optimizer::cpu_state(c,arc::ExactStateCache::Pipeline,&p,sizeof(p)))return;if(observing)profile::pipeline(c,p);{mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_pipeline(c,p);});}if(observing){pixel::pipeline(c,p);mirror::pipeline(c,p);optimizer::pipeline(c,p);if(detailed_tracking.load(std::memory_order_relaxed))runtime::pipeline(c,p);}}
void STDMETHODCALLTYPE targets(ID3D12GraphicsCommandList* c,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* h,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_targets(c,count,h,contiguous,depth);});if(observe_api())mirror::record(original_targets,c,count,h,contiguous,depth);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::targets(c,count,h,contiguous,depth);}
void STDMETHODCALLTYPE viewport(ID3D12GraphicsCommandList* c,UINT count,const D3D12_VIEWPORT* v){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observing=observe_api();const bool skip=observing&&count<=16&&optimizer::cpu_state(c,arc::ExactStateCache::Viewports,v,std::size_t(count)*sizeof(*v));if(!skip)arc::original_cpu_call([&]{return original_viewport(c,count,v);});if(observing&&detailed_tracking.load(std::memory_order_relaxed))runtime::viewport(c,count,v);}
void STDMETHODCALLTYPE scissor(ID3D12GraphicsCommandList* c,UINT count,const D3D12_RECT* v){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);const bool observing=observe_api();const bool skip=observing&&count<=16&&optimizer::cpu_state(c,arc::ExactStateCache::Scissors,v,std::size_t(count)*sizeof(*v));if(!skip)arc::original_cpu_call([&]{return original_scissor(c,count,v);});if(observing&&detailed_tracking.load(std::memory_order_relaxed))runtime::scissor(c,count,v);}
void STDMETHODCALLTYPE bind_heaps(ID3D12GraphicsCommandList* c,UINT count,ID3D12DescriptorHeap*const* h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_bind_heaps(c,count,h);});if(observe_api()){pixel::heaps(c,count,h);optimizer::heaps(c,count,h);}if(observe_api())mirror::record(original_bind_heaps,c,count,h);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::descriptor_heaps(c,count,h);}
void STDMETHODCALLTYPE graphics_table(ID3D12GraphicsCommandList* c,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_graphics_table(c,parameter,h);});if(observe_api()){pixel::table(c,parameter,h);mirror::record(original_graphics_table,c,parameter,h);}if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::root_table(c,false,parameter,h);}
void STDMETHODCALLTYPE compute_table(ID3D12GraphicsCommandList* c,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE h){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_compute_table(c,parameter,h);});if(observe_api())optimizer::table(c,parameter,h);if(observe_api())mirror::record(original_compute_table,c,parameter,h);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::root_table(c,true,parameter,h);}
void STDMETHODCALLTYPE copy(ID3D12GraphicsCommandList* c,ID3D12Resource* dst,ID3D12Resource* src){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_copy(c,dst,src);});if(observe_api())mirror::record(original_copy,c,dst,src);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::copy(c,src,dst,0,true);}
void STDMETHODCALLTYPE copy_buffer(ID3D12GraphicsCommandList* c,ID3D12Resource* dst,UINT64 dst_offset,ID3D12Resource* src,UINT64 src_offset,UINT64 bytes){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_copy_buffer(c,dst,dst_offset,src,src_offset,bytes);});if(observe_api())mirror::record(original_copy_buffer,c,dst,dst_offset,src,src_offset,bytes);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::copy(c,src,dst,bytes,false);}
void STDMETHODCALLTYPE clear(ID3D12GraphicsCommandList* c,D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT color[4],UINT count,const D3D12_RECT* rects){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_clear(c,h,color,count,rects);});if(observe_api())mirror::record(original_clear,c,h,color,count,rects);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::clear_target(c,h,count==0);}
void STDMETHODCALLTYPE graphics_root(ID3D12GraphicsCommandList* c,ID3D12RootSignature* root){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_graphics_root(c,root);});if(observe_api()){pixel::root(c,root);mirror::record(original_graphics_root,c,root);}if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::root_signature(c,false,root);}
void STDMETHODCALLTYPE compute_root(ID3D12GraphicsCommandList* c,ID3D12RootSignature* root){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);arc::original_cpu_call([&]{return original_compute_root(c,root);});if(observe_api())optimizer::root(c,root);if(observe_api())mirror::record(original_compute_root,c,root);if(observe_api())if(detailed_tracking.load(std::memory_order_relaxed))runtime::root_signature(c,true,root);}

using SignatureFn=decltype(ID3D12DeviceVtbl::CreateCommandSignature);SignatureFn original_signature{};
HRESULT STDMETHODCALLTYPE command_signature(ID3D12Device* d,const D3D12_COMMAND_SIGNATURE_DESC* desc,ID3D12RootSignature* root,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    const auto result=arc::original_cpu_call([&]{return original_signature(d,desc,root,iid,out);});
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12CommandSignature* signature=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12CommandSignature,reinterpret_cast<void**>(&signature)))){mirror::signature_created(signature,desc);profile::signature_created(signature,desc);optimizer::signature_created(signature,desc,root);ID3D12CommandSignature_Release(signature);}}return result;
}
using GraphicsPsoFn=decltype(ID3D12DeviceVtbl::CreateGraphicsPipelineState);GraphicsPsoFn original_graphics_pso{};
HRESULT STDMETHODCALLTYPE graphics_pso(ID3D12Device* d,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())optimizer::cpu_objects_changed();
    const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_graphics_pso(d,desc,iid,out);});}();
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12PipelineState* p=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12PipelineState,reinterpret_cast<void**>(&p)))){pixel::created(p,desc);mirror::pipeline_created(p,desc);profile::graphics_created(p,desc);ID3D12PipelineState_Release(p);}}return result;
}
using ComputePsoFn=decltype(ID3D12DeviceVtbl::CreateComputePipelineState);ComputePsoFn original_compute_pso{};
HRESULT STDMETHODCALLTYPE compute_pso(ID3D12Device* d,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())optimizer::cpu_objects_changed();
    const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_compute_pso(d,desc,iid,out);});}();
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12PipelineState* p=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12PipelineState,reinterpret_cast<void**>(&p)))){profile::compute_created(p,desc);optimizer::compute_created(p,desc);ID3D12PipelineState_Release(p);}}return result;
}
using StreamPsoFn=decltype(ID3D12Device2Vtbl::CreatePipelineState);StreamPsoFn original_stream_pso{};
HRESULT STDMETHODCALLTYPE stream_pso(ID3D12Device2* d,const D3D12_PIPELINE_STATE_STREAM_DESC* desc,REFIID iid,void** out){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())optimizer::cpu_objects_changed();
    const auto result=[&]{mirror::InternalCall native_call;return arc::original_cpu_call([&]{return original_stream_pso(d,desc,iid,out);});}();
    if(observe_api()&&SUCCEEDED(result)&&out&&*out){ID3D12PipelineState* p=nullptr;if(SUCCEEDED(IUnknown_QueryInterface(reinterpret_cast<IUnknown*>(*out),IID_ID3D12PipelineState,reinterpret_cast<void**>(&p)))){mirror::pipeline_stream_created(p,desc);profile::stream_created(p,desc);optimizer::stream_created(p,desc);ID3D12PipelineState_Release(p);}}return result;
}
using RateFn=decltype(ID3D12GraphicsCommandList5Vtbl::RSSetShadingRate);RateFn original_rate{};
void STDMETHODCALLTYPE rate(ID3D12GraphicsCommandList5* c,D3D12_SHADING_RATE value,const D3D12_SHADING_RATE_COMBINER* combiners){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())mirror::shading_rate(reinterpret_cast<ID3D12GraphicsCommandList*>(c),value,combiners);arc::original_cpu_call([&]{return original_rate(c,value,combiners);});
}
using ImageRateFn=decltype(ID3D12GraphicsCommandList5Vtbl::RSSetShadingRateImage);ImageRateFn original_image_rate{};
void STDMETHODCALLTYPE image_rate(ID3D12GraphicsCommandList5* c,ID3D12Resource* image){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api())mirror::shading_image(reinterpret_cast<ID3D12GraphicsCommandList*>(c),image);arc::original_cpu_call([&]{return original_image_rate(c,image);});
}
using BeginPassFn=decltype(ID3D12GraphicsCommandList4Vtbl::BeginRenderPass);BeginPassFn original_begin_pass{};
using EndPassFn=decltype(ID3D12GraphicsCommandList4Vtbl::EndRenderPass);EndPassFn original_end_pass{};
void STDMETHODCALLTYPE begin_pass(ID3D12GraphicsCommandList4* c,UINT count,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,D3D12_RENDER_PASS_FLAGS flags){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    if(observe_api()){if(flags&(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS|D3D12_RENDER_PASS_FLAG_RESUMING_PASS))pixel::invalidate(reinterpret_cast<ID3D12GraphicsCommandList*>(c));profile::render_pass(reinterpret_cast<ID3D12GraphicsCommandList*>(c),true,flags);optimizer::render_pass(reinterpret_cast<ID3D12GraphicsCommandList*>(c),true,flags);}
    {mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_begin_pass(c,count,targets,depth,flags);});}
    if(observe_api()&&detailed_tracking)runtime::unsupported();
}
void STDMETHODCALLTYPE end_pass(ID3D12GraphicsCommandList4* c){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
    {mirror::InternalCall native_call;arc::original_cpu_call([&]{return original_end_pass(c);});}
    if(observe_api()){profile::render_pass(reinterpret_cast<ID3D12GraphicsCommandList*>(c),false);optimizer::render_pass(reinterpret_cast<ID3D12GraphicsCommandList*>(c),false);if(detailed_tracking)runtime::unsupported();}
}
template<int Tag,class Fn,bool Copy>struct ExtraCommandHook;
template<int Tag,class R,class Self,class... Args,bool Copy>
struct ExtraCommandHook<Tag,R(STDMETHODCALLTYPE*)(Self*,Args...),Copy>{
    using Fn=R(STDMETHODCALLTYPE*)(Self*,Args...);static inline Fn original{};
    static R STDMETHODCALLTYPE call(Self* self,Args... args){static const auto cpu_site=arc::InterceptCpuMeter::register_site(__FUNCSIG__);arc::InterceptCpuMeter::Scope cpu_hook(!mirror::internal()&&!arc::dx12::cpu_cost::on_worker_thread(),cpu_site);
        if(observe_api()){
            if constexpr(Tag==3||Tag==5){auto values=std::tuple{args...};const auto value=std::get<0>(values);if(optimizer::cpu_state(reinterpret_cast<ID3D12GraphicsCommandList*>(self),Tag==3?arc::ExactStateCache::Topology:arc::ExactStateCache::Stencil,&value,sizeof(value)))return;}
            else if constexpr(Tag==4){auto values=std::tuple{args...};const auto* value=std::get<0>(values);const float defaults[]{1,1,1,1};if(optimizer::cpu_state(reinterpret_cast<ID3D12GraphicsCommandList*>(self),arc::ExactStateCache::Blend,value?value:defaults,sizeof(defaults)))return;}
            if constexpr(Tag==7){auto values=std::tuple{args...};pixel::constants(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),1,&std::get<1>(values),std::get<2>(values));}
            else if constexpr(Tag==9){auto values=std::tuple{args...};pixel::constants(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),std::get<1>(values),static_cast<const UINT*>(std::get<2>(values)),std::get<3>(values));}
            else if constexpr(Tag==11||Tag==13||Tag==15){auto values=std::tuple{args...};pixel::descriptor(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),Tag==11?D3D12_ROOT_PARAMETER_TYPE_CBV:Tag==13?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV,std::get<1>(values));}
            if constexpr(Tag==6){auto values=std::tuple{args...};optimizer::constants(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),1,&std::get<1>(values),std::get<2>(values));}
            else if constexpr(Tag==8){auto values=std::tuple{args...};optimizer::constants(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),std::get<1>(values),static_cast<const UINT*>(std::get<2>(values)),std::get<3>(values));}
            else if constexpr(Tag==10||Tag==12||Tag==14){auto values=std::tuple{args...};optimizer::descriptor(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values),Tag==10?D3D12_ROOT_PARAMETER_TYPE_CBV:Tag==12?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV,std::get<1>(values));}
            else if constexpr(Tag==26){auto values=std::tuple{args...};optimizer::predication(reinterpret_cast<ID3D12GraphicsCommandList*>(self),std::get<0>(values)!=nullptr);}
            else if constexpr(Tag==23||Tag==24){auto values=std::tuple{args...};if(std::get<1>(values)!=D3D12_QUERY_TYPE_TIMESTAMP)optimizer::query_scope(reinterpret_cast<ID3D12GraphicsCommandList*>(self),Tag==23);}
            else if constexpr(Tag==301)optimizer::invalidate(reinterpret_cast<ID3D12GraphicsCommandList*>(self));
            else if constexpr(!Copy)optimizer::state_unknown(reinterpret_cast<ID3D12GraphicsCommandList*>(self));
            if constexpr(Copy){mirror::record(original,self,args...);if(detailed_tracking.load(std::memory_order_relaxed))runtime::unsupported();}
            else{if constexpr(Tag==301)profile::protected_session(reinterpret_cast<ID3D12GraphicsCommandList*>(self));else profile::opaque(reinterpret_cast<ID3D12GraphicsCommandList*>(self));pixel::invalidate(reinterpret_cast<ID3D12GraphicsCommandList*>(self));mirror::invalidate(reinterpret_cast<ID3D12GraphicsCommandList*>(self));if(detailed_tracking.load(std::memory_order_relaxed))runtime::unsupported();}
        }
        return arc::original_cpu_call([&]{return original(self,args...);});
    }
};

template<class T>bool install(T target,T replacement,T* original){
    // Trampolines outlive the bootstrap device; keep the owning runtime module
    // loaded for the process lifetime, just like the probe itself.
    if(installed_count>=installed_targets.size()){++hook_failures;return false;}HMODULE owner{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(target),&owner)){++hook_failures;return false;}
    const auto status=MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(original));
    if(status!=MH_OK){++hook_failures;return false;}installed_targets[installed_count++]=reinterpret_cast<void*>(target);return true;
}
template<class T>bool install_detailed(T target,T replacement,T* original){
    const bool ok=install(target,replacement,original);if(ok)detailed_only_targets.insert(reinterpret_cast<void*>(target));return ok;
}
template<class T>bool install_optimizer(T target,T replacement,T* original){
    const bool ok=install_detailed(target,replacement,original);if(ok)optimizer_targets.insert(reinterpret_cast<void*>(target));return ok;
}
bool set_passive_hooks(bool passive){
    std::lock_guard lock(hook_mode_mutex);
    if(passive){optimizer::invalidate_all();runtime::invalidate_color_spaces();}
    // Present, Present1 and ExecuteCommandLists stay installed. Cached lists
    // can contain a rate image, which must be neutralized even in passive mode.
    const bool raster=!passive&&(detailed_tracking||raster_setup||mirror::requested_rate()||pixel::requested_steps()||profile::needs_raster_observation());
    for(std::size_t i=3;i<installed_count;++i){const auto target=installed_targets[i];const bool enabled=raster_targets.contains(target)?raster:!passive&&(detailed_tracking||!detailed_only_targets.contains(target)||(optimizer::enabled()&&optimizer_targets.contains(target)));const auto result=enabled?MH_QueueEnableHook(target):MH_QueueDisableHook(target);if(result!=MH_OK)return false;}
    if(MH_ApplyQueued()!=MH_OK)return false;passive_hooks=passive;raster_hooks_enabled=raster;return true;
}
void refresh_raster_hooks(){
    std::lock_guard lock(hook_mode_mutex);
    const bool needed=!passive_hooks&&(detailed_tracking||raster_setup||mirror::requested_rate()||pixel::requested_steps()||profile::needs_raster_observation());
    if(needed==raster_hooks_enabled)return;
    for(const auto target:raster_targets)if((needed?MH_QueueEnableHook(target):MH_QueueDisableHook(target))!=MH_OK){++hook_failures;return;}
    if(MH_ApplyQueued()!=MH_OK){++hook_failures;return;}raster_hooks_enabled=needed;
}

template<int Tag,bool Copy,class Fn>bool install_extra(Fn target){using Hook=ExtraCommandHook<Tag,Fn,Copy>;if constexpr(Tag==3||Tag==4||Tag==5||Tag==6||Tag==8||Tag==10||Tag==12||Tag==14||Tag==23||Tag==24||Tag==26)return install_optimizer(target,Hook::call,&Hook::original);else if constexpr(Copy)return install_detailed(target,Hook::call,&Hook::original);else return install(target,Hook::call,&Hook::original);}

void snapshot(){
    std::lock_guard snapshot_lock(output_mutex);
    // No reference replay/readback or safe mutation capability has been established
    // for an unknown process. The portable policy must therefore abstain.
    arc::PerceptualTrialController policy;
    const auto proposal=policy.choose({});
    const auto temporary=output.wstring()+L".tmp";
    std::ofstream file(temporary,std::ios::trunc);
    file<<"{\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"initialized\":"<<(initialized.load()?"true":"false")
        <<",\"mode\":\""<<(mirror::requested_rate()?"experimental_vrs":"observe_only")<<"\",\"engine_specific_code\":false,\"telemetry_scope\":\"partial_objects_descriptors_work_and_images\","
          "\"replay_reference_available\":false,\"safe_mutation_capability\":false,\"quality_mutations\":"<<mirror::modified_draws()<<","
          "\"policy_abstained\":"<<(!proposal?"true":"false")<<",\"present_calls\":"<<presents.load()
        <<",\"present_failures\":"<<present_failures.load()<<",\"queue_submits\":"<<submits.load()
        <<",\"last_present_hresult\":"<<last_present_error.load()<<",\"last_present_sync\":"<<last_present_sync.load()<<",\"last_present_flags\":"<<last_present_flags.load()
        <<",\"device_reason_available\":"<<(device_reason_available.load()?"true":"false")<<",\"last_device_removed_reason\":"<<last_device_reason.load()
        <<",\"submitted_lists\":"<<lists.load()<<",\"draw_calls\":"<<draws.load()
        <<",\"indexed_draw_calls\":"<<indexed.load()<<",\"dispatch_calls\":"<<dispatches.load()
        <<",\"committed_resources\":"<<resources.load()<<",\"hook_failures\":"<<hook_failures.load()
        <<",\"runtime\":";runtime::snapshot(file);file<<",\"pixel_optimizer\":";pixel::snapshot(file);file<<",\"command_mirror\":";mirror::snapshot(file);file<<",\"gpu_profile\":";profile::snapshot(file);file<<",\"optimizer\":";optimizer::snapshot(file);file<<",\"optimizer_cpu\":";optimizer::cpu_snapshot(file);file<<",\"cpu_state_cache\":";optimizer::cpu_cache_snapshot(file);file<<",\"interceptor_cpu\":";optimizer::intercept_cpu_snapshot(file);file<<",\"worker_placement\":";arc::dx12::placement::snapshot(file);file<<",\"optimizer_gpu_control\":";optimizer::control_timing_snapshot(file);file<<",\"optimizer_coverage\":";optimizer::coverage_snapshot(file);file<<",\"automatic_session\":";autotune::snapshot(file);file<<",\"optimizer_calibration\":";optimizer::calibration_snapshot(file);
    file<<",\"raster_draw_hooks_enabled\":"<<(raster_hooks_enabled?"true":"false")<<",\"render_hooks_passive\":"<<(passive_hooks?"true":"false")<<",\"detailed_tracking_enabled\":"<<(detailed_tracking?"true":"false")<<",\"runtime_object_snapshot_current\":"<<(detailed_tracking?"true":"false")<<",\"coverage_complete\":false,\"note\":\"Object/descriptor lifetime tracking and bounded submitted-work capture. Shader accesses are possible candidates; experimental VRS command substitution is separate from the perceptual controller; no comparable replay reference is established.\"}\n";
    file.close();MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING);
}
DWORD WINAPI logger(void*){
    arc::dx12::cpu_cost::Registration worker_cpu;
    unsigned tick=0;for(;;){try{mirror::collect();profile::collect();refresh_raster_hooks();optimizer::collect();if(arc::dx12::placement::adaptive()){const auto placement_epoch=optimizer::policy_stamp();arc::dx12::placement::collect(placement_epoch.epoch,placement_epoch.selected_pipeline);}runtime::flush_image();runtime::flush_timing();if(tick++%100==0){runtime::flush_capture();snapshot();}}catch(...){}Sleep(10);}return 0;
}
}

namespace arc::dx12::hooks {
bool begin_raster_observation()noexcept{++raster_setup;try{if(set_passive_hooks(false))return true;}catch(...){}--raster_setup;return false;}
void end_raster_observation()noexcept{--raster_setup;}
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
        ok=install(reinterpret_cast<ResizeFn>(swapchain->lpVtbl->ResizeBuffers),resize,&original_resize)&&ok;
        ok=install(reinterpret_cast<FullscreenFn>(swapchain->lpVtbl->SetFullscreenState),fullscreen,&original_fullscreen)&&ok;
        {IDXGISwapChain3* extra=nullptr;if(SUCCEEDED(IDXGISwapChain1_QueryInterface(swapchain,IID_IDXGISwapChain3,reinterpret_cast<void**>(&extra)))){ok=install(extra->lpVtbl->SetColorSpace1,color_space,&original_color_space)&&ok;ok=install(extra->lpVtbl->ResizeBuffers1,resize1,&original_resize1)&&ok;IDXGISwapChain3_Release(extra);}}
        ok=install(command->lpVtbl->DrawInstanced,draw,&original_draw)&&ok;
        ok=install(command->lpVtbl->DrawIndexedInstanced,draw_indexed,&original_indexed)&&ok;
        raster_targets.insert(reinterpret_cast<void*>(command->lpVtbl->DrawInstanced));raster_targets.insert(reinterpret_cast<void*>(command->lpVtbl->DrawIndexedInstanced));
        ok=install(command->lpVtbl->Dispatch,dispatch,&original_dispatch)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateCommittedResource,resource,&original_resource)&&ok;
        ok=install_optimizer(device->lpVtbl->CreatePlacedResource,placed,&original_placed)&&ok;
        ok=install(device->lpVtbl->CreateRootSignature,root_create,&original_root_create)&&ok;
        {ID3D12Device4* extra=nullptr;if(SUCCEEDED(ID3D12Device_QueryInterface(device,IID_ID3D12Device4,reinterpret_cast<void**>(&extra)))){ok=install_optimizer(extra->lpVtbl->CreateCommittedResource1,resource1,&original_resource1)&&ok;ID3D12Device4_Release(extra);}}
        ok=install(factory->lpVtbl->CreateSwapChainForHwnd,swap_hwnd,&original_swap_hwnd)&&ok;
        ok=install(reinterpret_cast<SwapFn>(factory->lpVtbl->CreateSwapChain),create_swap,&original_swap)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateDescriptorHeap,heap,&original_heap)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateShaderResourceView,srv,&original_srv)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateUnorderedAccessView,uav,&original_uav)&&ok;
        ok=install_detailed(device->lpVtbl->CreateRenderTargetView,rtv,&original_rtv)&&ok;
        ok=install_detailed(device->lpVtbl->CreateDepthStencilView,dsv,&original_dsv)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateSampler,sampler,&original_sampler)&&ok;
        ok=install_optimizer(device->lpVtbl->CreateConstantBufferView,cbv,&original_cbv)&&ok;
        ok=install_optimizer(device->lpVtbl->CopyDescriptorsSimple,descriptors,&original_descriptors)&&ok;
        ok=install_optimizer(device->lpVtbl->CopyDescriptors,descriptor_ranges,&original_descriptor_ranges)&&ok;
        ok=install_detailed(queue->lpVtbl->Signal,signal,&original_signal)&&ok;
        ok=install_detailed(queue->lpVtbl->Wait,wait,&original_wait)&&ok;
        ok=install(command->lpVtbl->ExecuteIndirect,indirect,&original_indirect)&&ok;
        ok=install(command->lpVtbl->ExecuteBundle,bundle,&original_bundle)&&ok;
        ok=install_detailed(command->lpVtbl->ResourceBarrier,barriers,&original_barriers)&&ok;
        ok=install(command->lpVtbl->Reset,reset,&original_reset)&&ok;
        ok=install(device->lpVtbl->CreateCommandList,create_command,&original_create_command)&&ok;
        ok=install(command->lpVtbl->Close,close,&original_close)&&ok;
        ok=install(command->lpVtbl->SetPipelineState,pipeline,&original_pipeline)&&ok;
        ok=install_detailed(command->lpVtbl->OMSetRenderTargets,targets,&original_targets)&&ok;
        ok=install_optimizer(command->lpVtbl->RSSetViewports,viewport,&original_viewport)&&ok;
        ok=install_optimizer(command->lpVtbl->RSSetScissorRects,scissor,&original_scissor)&&ok;
        ok=install_optimizer(command->lpVtbl->SetDescriptorHeaps,bind_heaps,&original_bind_heaps)&&ok;
        ok=install_optimizer(command->lpVtbl->SetGraphicsRootDescriptorTable,graphics_table,&original_graphics_table)&&ok;
        ok=install_optimizer(command->lpVtbl->SetComputeRootDescriptorTable,compute_table,&original_compute_table)&&ok;
        ok=install_optimizer(command->lpVtbl->SetGraphicsRootSignature,graphics_root,&original_graphics_root)&&ok;
        ok=install_optimizer(command->lpVtbl->SetComputeRootSignature,compute_root,&original_compute_root)&&ok;
        ok=install_detailed(command->lpVtbl->CopyResource,copy,&original_copy)&&ok;
        ok=install_detailed(command->lpVtbl->CopyBufferRegion,copy_buffer,&original_copy_buffer)&&ok;
        ok=install_detailed(command->lpVtbl->ClearRenderTargetView,clear,&original_clear)&&ok;
        bool mirror_ok=true;
        mirror_ok=install(device->lpVtbl->CreateCommandSignature,command_signature,&original_signature)&&mirror_ok;
        {ID3D12Device2* extra=nullptr;if(SUCCEEDED(ID3D12Device_QueryInterface(device,IID_ID3D12Device2,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install(extra->lpVtbl->CreatePipelineState,stream_pso,&original_stream_pso)&&mirror_ok;ID3D12Device2_Release(extra);}}

        mirror_ok=install(device->lpVtbl->CreateComputePipelineState,compute_pso,&original_compute_pso)&&mirror_ok;
        mirror_ok=install(device->lpVtbl->CreateGraphicsPipelineState,graphics_pso,&original_graphics_pso)&&mirror_ok;
        {ID3D12Device8* modern=nullptr;if(SUCCEEDED(ID3D12Device_QueryInterface(device,IID_ID3D12Device8,reinterpret_cast<void**>(&modern)))){
            mirror_ok=install_optimizer(modern->lpVtbl->CreateSamplerFeedbackUnorderedAccessView,feedback_view,&original_feedback_view)&&mirror_ok;ID3D12Device8_Release(modern);}}
        mirror_ok=install_extra<0,true>(command->lpVtbl->CopyTextureRegion)&&mirror_ok;
        mirror_ok=install_extra<1,true>(command->lpVtbl->CopyTiles)&&mirror_ok;
        mirror_ok=install_extra<2,true>(command->lpVtbl->ResolveSubresource)&&mirror_ok;
        mirror_ok=install_extra<3,true>(command->lpVtbl->IASetPrimitiveTopology)&&mirror_ok;
        mirror_ok=install_extra<4,true>(command->lpVtbl->OMSetBlendFactor)&&mirror_ok;
        mirror_ok=install_extra<5,true>(command->lpVtbl->OMSetStencilRef)&&mirror_ok;
        mirror_ok=install_extra<6,true>(command->lpVtbl->SetComputeRoot32BitConstant)&&mirror_ok;
        mirror_ok=install_extra<7,true>(command->lpVtbl->SetGraphicsRoot32BitConstant)&&mirror_ok;
        mirror_ok=install_extra<8,true>(command->lpVtbl->SetComputeRoot32BitConstants)&&mirror_ok;
        mirror_ok=install_extra<9,true>(command->lpVtbl->SetGraphicsRoot32BitConstants)&&mirror_ok;
        mirror_ok=install_extra<10,true>(command->lpVtbl->SetComputeRootConstantBufferView)&&mirror_ok;
        mirror_ok=install_extra<11,true>(command->lpVtbl->SetGraphicsRootConstantBufferView)&&mirror_ok;
        mirror_ok=install_extra<12,true>(command->lpVtbl->SetComputeRootShaderResourceView)&&mirror_ok;
        mirror_ok=install_extra<13,true>(command->lpVtbl->SetGraphicsRootShaderResourceView)&&mirror_ok;
        mirror_ok=install_extra<14,true>(command->lpVtbl->SetComputeRootUnorderedAccessView)&&mirror_ok;
        mirror_ok=install_extra<15,true>(command->lpVtbl->SetGraphicsRootUnorderedAccessView)&&mirror_ok;
        mirror_ok=install_extra<16,true>(command->lpVtbl->IASetIndexBuffer)&&mirror_ok;
        mirror_ok=install_extra<17,true>(command->lpVtbl->IASetVertexBuffers)&&mirror_ok;
        mirror_ok=install_extra<18,true>(command->lpVtbl->SOSetTargets)&&mirror_ok;
        mirror_ok=install_extra<19,true>(command->lpVtbl->ClearDepthStencilView)&&mirror_ok;
        mirror_ok=install_extra<20,true>(command->lpVtbl->ClearUnorderedAccessViewUint)&&mirror_ok;
        mirror_ok=install_extra<21,true>(command->lpVtbl->ClearUnorderedAccessViewFloat)&&mirror_ok;
        mirror_ok=install_extra<22,true>(command->lpVtbl->DiscardResource)&&mirror_ok;
        mirror_ok=install_extra<23,true>(command->lpVtbl->BeginQuery)&&mirror_ok;
        mirror_ok=install_extra<24,true>(command->lpVtbl->EndQuery)&&mirror_ok;
        mirror_ok=install_extra<25,true>(command->lpVtbl->ResolveQueryData)&&mirror_ok;
        mirror_ok=install_extra<26,true>(command->lpVtbl->SetPredication)&&mirror_ok;
        mirror_ok=install_extra<27,true>(command->lpVtbl->SetMarker)&&mirror_ok;
        mirror_ok=install_extra<28,true>(command->lpVtbl->BeginEvent)&&mirror_ok;
        mirror_ok=install_extra<29,true>(command->lpVtbl->EndEvent)&&mirror_ok;
        mirror_ok=install_extra<100,false>(command->lpVtbl->ClearState)&&mirror_ok;
        {ID3D12GraphicsCommandList1* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList1,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<101,false>(extra->lpVtbl->AtomicCopyBufferUINT)&&mirror_ok;
            mirror_ok=install_extra<102,false>(extra->lpVtbl->AtomicCopyBufferUINT64)&&mirror_ok;
            mirror_ok=install_extra<103,false>(extra->lpVtbl->OMSetDepthBounds)&&mirror_ok;
            mirror_ok=install_extra<104,false>(extra->lpVtbl->SetSamplePositions)&&mirror_ok;
            mirror_ok=install_extra<105,false>(extra->lpVtbl->ResolveSubresourceRegion)&&mirror_ok;
            mirror_ok=install_extra<106,false>(extra->lpVtbl->SetViewInstanceMask)&&mirror_ok;
            ID3D12GraphicsCommandList1_Release(extra);}}
        {ID3D12GraphicsCommandList2* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList2,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<201,false>(extra->lpVtbl->WriteBufferImmediate)&&mirror_ok;
            ID3D12GraphicsCommandList2_Release(extra);}}
        {ID3D12GraphicsCommandList3* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList3,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<301,false>(extra->lpVtbl->SetProtectedResourceSession)&&mirror_ok;
            ID3D12GraphicsCommandList3_Release(extra);}}
        {ID3D12GraphicsCommandList4* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList4,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install(extra->lpVtbl->BeginRenderPass,begin_pass,&original_begin_pass)&&mirror_ok;
            mirror_ok=install(extra->lpVtbl->EndRenderPass,end_pass,&original_end_pass)&&mirror_ok;
            mirror_ok=install_extra<403,false>(extra->lpVtbl->InitializeMetaCommand)&&mirror_ok;
            mirror_ok=install_extra<404,false>(extra->lpVtbl->ExecuteMetaCommand)&&mirror_ok;
            mirror_ok=install_extra<405,false>(extra->lpVtbl->BuildRaytracingAccelerationStructure)&&mirror_ok;
            mirror_ok=install_extra<406,false>(extra->lpVtbl->EmitRaytracingAccelerationStructurePostbuildInfo)&&mirror_ok;
            mirror_ok=install_extra<407,false>(extra->lpVtbl->CopyRaytracingAccelerationStructure)&&mirror_ok;
            mirror_ok=install_extra<408,false>(extra->lpVtbl->SetPipelineState1)&&mirror_ok;
            mirror_ok=install_extra<409,false>(extra->lpVtbl->DispatchRays)&&mirror_ok;
            ID3D12GraphicsCommandList4_Release(extra);}}
        {ID3D12GraphicsCommandList5* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList5,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install(extra->lpVtbl->RSSetShadingRateImage,image_rate,&original_image_rate)&&mirror_ok;
            mirror_ok=install(extra->lpVtbl->RSSetShadingRate,rate,&original_rate)&&mirror_ok;
            ID3D12GraphicsCommandList5_Release(extra);}}
        {ID3D12GraphicsCommandList6* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList6,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<601,false>(extra->lpVtbl->DispatchMesh)&&mirror_ok;
            ID3D12GraphicsCommandList6_Release(extra);}}
        {ID3D12GraphicsCommandList7* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList7,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<701,true>(extra->lpVtbl->Barrier)&&mirror_ok;
            ID3D12GraphicsCommandList7_Release(extra);}}
        {ID3D12GraphicsCommandList8* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList8,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<801,false>(extra->lpVtbl->OMSetFrontAndBackStencilRef)&&mirror_ok;
            ID3D12GraphicsCommandList8_Release(extra);}}
        {ID3D12GraphicsCommandList9* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList9,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<901,false>(extra->lpVtbl->RSSetDepthBias)&&mirror_ok;
            mirror_ok=install_extra<902,false>(extra->lpVtbl->IASetIndexBufferStripCutValue)&&mirror_ok;
            ID3D12GraphicsCommandList9_Release(extra);}}
        {ID3D12GraphicsCommandList10* extra=nullptr;if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(command,IID_ID3D12GraphicsCommandList10,reinterpret_cast<void**>(&extra)))){
            mirror_ok=install_extra<1001,false>(extra->lpVtbl->SetProgram)&&mirror_ok;
            mirror_ok=install_extra<1002,false>(extra->lpVtbl->DispatchGraph)&&mirror_ok;
            ID3D12GraphicsCommandList10_Release(extra);}}
        mirror_hooks_ready=mirror_ok;
        if(!ok)break;
        if(!arc::dx12::children::install())break;
        if(MH_EnableHook(MH_ALL_HOOKS)!=MH_OK)break;
        result=0;
    }while(false);
    if(swapchain)IDXGISwapChain1_Release(swapchain);if(factory)IDXGIFactory2_Release(factory);
    if(command)ID3D12GraphicsCommandList_Release(command);if(allocator)ID3D12CommandAllocator_Release(allocator);
    if(queue)ID3D12CommandQueue_Release(queue);if(device)ID3D12Device_Release(device);
    if(window)DestroyWindow(window);UnregisterClassW(L"ARCReadOnlyBootstrap",module);
    if(result){MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();try{snapshot();}catch(...){}return result;}
    arc::dx12::placement::initialize();
    if(!optimizer::initialize())return 8;
    initialized=true;recording=true;HANDLE thread=CreateThread(nullptr,0,logger,nullptr,0,nullptr);
    if(!thread){recording=false;return 6;}CloseHandle(thread);
    wchar_t automatic_config[32768]{};const auto config_size=GetEnvironmentVariableW(L"ARC_AUTO_CONFIG",automatic_config,32768);
    if(config_size&&config_size<32768&&!autotune::start(automatic_config))return 9;
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){module=instance;DisableThreadLibraryCalls(instance);}return TRUE;
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcBeginCapture(void*){if(!detailed_tracking)return 2;runtime::begin_capture();return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcEndCapture(void* path){if(!path)return 1;try{runtime::end_capture(static_cast<const wchar_t*>(path));return 0;}catch(...){return 2;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcSnapshot(void*){try{snapshot();return 0;}catch(...){return 1;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcSetMetricsPath(void* value){
    if(!value||autotune::active())return 1;try{const std::filesystem::path path(static_cast<const wchar_t*>(value));
        if(!path.is_absolute()||std::filesystem::exists(path)||!std::filesystem::is_directory(path.parent_path()))return 2;
        std::lock_guard lock(output_mutex);output=path;return 0;
    }catch(...){return 3;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestFrame(void* path){if(autotune::active())return 10;if(!path)return 1;if(!detailed_tracking)return 4;try{return runtime::request_frame(static_cast<const wchar_t*>(path))?0:2;}catch(...){return 3;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestImage(void* path){if(autotune::active())return 10;if(!path)return 1;try{return runtime::request_image(static_cast<const wchar_t*>(path))?0:2;}catch(...){return 3;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestFeatures(void* path){if(autotune::active())return 10;if(!path)return 1;try{return runtime::request_image(static_cast<const wchar_t*>(path),true)?0:2;}catch(...){return 3;}}
extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestTiming(void* path){if(autotune::active())return 10;if(!path)return 1;try{std::wstring request=static_cast<const wchar_t*>(path);UINT seconds=60;const auto split=request.find(L'|');if(split!=std::wstring::npos){std::size_t used=0;const auto value=std::stoul(request.substr(0,split),&used);if(used!=split||value<1||value>60)return 4;seconds=static_cast<UINT>(value);request=request.substr(split+1);}return runtime::request_timing(request,seconds)?0:2;}catch(...){return 3;}}

extern "C" __declspec(dllexport) DWORD WINAPI ArcSetPixelMipSteps(void* value){if(autotune::active()||!value||!optimizer::enabled())return 1;try{const std::wstring text=static_cast<const wchar_t*>(value);std::size_t used{};const auto steps=std::stoul(text,&used);if(used!=text.size()||steps>8)return 2;if(!arc::dx12::hooks::begin_raster_observation())return 3;const bool ok=pixel::configure(steps);arc::dx12::hooks::end_raster_observation();return ok?0:4;}catch(...){return 2;}}

extern "C" __declspec(dllexport) DWORD WINAPI ArcExperimentalVrs(void* value){if(autotune::active())return 10;
    if(!value)return 1;const auto* mode=static_cast<const wchar_t*>(value);
    if(wcscmp(mode,L"off")==0)return mirror::configure(0)?0:2;
    if(wcscmp(mode,L"2x2")!=0||!mirror_hooks_ready)return 3;
    if(profile::busy())return 7;
    if(!arc::dx12::hooks::begin_raster_observation())return 5;
    const bool enabled=mirror::configure(D3D12_SHADING_RATE_2X2);arc::dx12::hooks::end_raster_observation();return enabled?0:4;
}

extern "C" __declspec(dllexport) DWORD WINAPI ArcUseLeanMode(void*){detailed_tracking=false;runtime::observation_mode_changed();return set_passive_hooks(passive_hooks)?0:1;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcUsePassiveMode(void*){if(autotune::active())return 10;if(profile::busy())return 2;pixel::configure(0);mirror::configure(0);detailed_tracking=false;runtime::observation_mode_changed();return set_passive_hooks(true)?0:1;}

extern "C" __declspec(dllexport) DWORD WINAPI ArcRequestGpuProfile(void* path){if(autotune::active())return 10;
    if(!path)return 1;if(mirror::requested_rate())return 5;if(!mirror_hooks_ready)return 7;
    try{std::wstring request=static_cast<const wchar_t*>(path);UINT frames=8;const auto split=request.find(L'|');
        if(split!=std::wstring::npos){std::size_t used=0;const auto value=std::stoul(request.substr(0,split),&used);if(used!=split||value<1||value>32)return 4;frames=static_cast<UINT>(value);request=request.substr(split+1);}
        if(!set_passive_hooks(false))return 6;return profile::request(request,frames)?0:2;
    }catch(...){return 3;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStopGpuProfile(void*){if(autotune::active())return 10;profile::stop();return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcExperimentalCompute(void* mode){if(autotune::active())return 10;if(!mode||!optimizer::enabled())return 1;if(!set_passive_hooks(false))return 3;return optimizer::configure(static_cast<const wchar_t*>(mode))?0:4;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStartOptimizer(void* config){if(!set_passive_hooks(false))return 3;return config&&autotune::start(static_cast<const wchar_t*>(config))?0:1;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcConfigureRuntime(void* config){
    if(!config||!autotune::configure_runtime(static_cast<const wchar_t*>(config)))return 1;
    if(!arc::dx12::children::configure(static_cast<const wchar_t*>(config)))return 4;
    if(!initialized)return 0;
    if(!optimizer::initialize())return 2;return set_passive_hooks(false)?0:3;
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcSetTargetFps(void* value){
    if(!value)return 1;try{const std::wstring text=static_cast<const wchar_t*>(value);std::size_t used{};const auto fps=std::stod(text,&used);return used==text.size()&&autotune::target(fps)?0:2;}catch(...){return 2;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcSetDiagnostics(void* value){if(!value)return 1;const auto* text=static_cast<const wchar_t*>(value);if(wcscmp(text,L"on")&&wcscmp(text,L"off"))return 2;autotune::diagnostics(wcscmp(text,L"on")==0);return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStopOptimizer(void*){arc::dx12::children::stop();autotune::stop();return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcExperimentalPolicy(void* path){if(autotune::active())return 10;return path&&optimizer::configure_bundle_file(static_cast<const wchar_t*>(path))?0:1;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcExperimentalCpuState(void* mode){if(autotune::active())return 10;
    if(!mode)return 1;const auto* text=static_cast<const wchar_t*>(mode);const bool enabled=wcscmp(text,L"on")==0;
    if(!enabled&&wcscmp(text,L"off")!=0)return 2;
    if(enabled&&!set_passive_hooks(false))return 3;
    return optimizer::cpu_configure(enabled)?0:4;
}

extern "C" __declspec(dllexport) DWORD WINAPI ArcWorkerPlacement(void* mode){if(autotune::active())return 10;return mode&&arc::dx12::placement::configure(static_cast<const wchar_t*>(mode))?0:1;}
