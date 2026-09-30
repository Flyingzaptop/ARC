#include "arc/arc2/frontend.hpp"
#include "observe.hpp"
#include "present_metrics.hpp"
#include "arc/arc2/runtime.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>
namespace arc::arc2 {
using Microsoft::WRL::ComPtr;
namespace {
class Factory;
HRESULT wrap_swapchain(IUnknown*,IUnknown*,IUnknown*,REFIID,void**);
class Swapchain final : public IDXGISwapChain4 {
 ComPtr<IDXGISwapChain4> native_; IUnknown* parent_{}; IUnknown* queue_{};
 std::atomic<ULONG> refs_{1}; ObjectId id_{}; std::mutex buffers_mutex_; std::map<UINT,ObjectId> buffers_; bool queue_uncertain_{};
 void resized(){std::lock_guard lock(buffers_mutex_);buffers_.clear();runtime().resize_swapchain(id_);}
 ObjectId current_buffer(){std::lock_guard lock(buffers_mutex_);auto it=buffers_.find(native_->GetCurrentBackBufferIndex());return it==buffers_.end()?ObjectId{}:it->second;}
 HRESULT after_present(HRESULT hr,UINT flags,ObjectId buffer){if(hr==S_OK&&!(flags&DXGI_PRESENT_TEST)){runtime().present(id_,queue_uncertain_?ObjectId{}:ObjectId{object_id(queue_)},buffer,hr);observe_tick();}return hr;}
public:
 Swapchain(IDXGISwapChain4* p,IUnknown* parent,IUnknown* queue):native_(p),parent_(parent),queue_(queue){parent_->AddRef();queue_->AddRef();id_=runtime().create_object(ObjectKind::Swapchain,reinterpret_cast<uintptr_t>(p));}
 ~Swapchain(){runtime().destroy_object(id_);queue_->Release();parent_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)||iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)||iid==__uuidof(IDXGISwapChain3)||iid==__uuidof(IDXGISwapChain4)){void* check{};auto hr=native_->QueryInterface(iid,&check);if(FAILED(hr))return hr;static_cast<IUnknown*>(check)->Release();*out=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;}
  runtime().unsupported({},"DXGI swapchain QueryInterface unsupported");return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release() override{auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** out) override{return parent_->QueryInterface(iid,out);}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out) override{if(!out)return E_POINTER;*out=nullptr;IUnknown* value{};auto hr=native_->GetDevice(iid,reinterpret_cast<void**>(&value));if(FAILED(hr))return hr;return wrap_object(value,iid,out);}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name,const IUnknown* value) override{return native_->SetPrivateDataInterface(name,value);}
 HRESULT STDMETHODCALLTYPE GetBuffer(UINT index,REFIID iid,void** out) override{if(!out)return E_POINTER;*out=nullptr;IUnknown* value{};auto hr=native_->GetBuffer(index,iid,reinterpret_cast<void**>(&value));if(FAILED(hr))return hr;auto wrapped=wrap_resource(value,iid,out);if(SUCCEEDED(wrapped)){std::lock_guard lock(buffers_mutex_);buffers_[index]=ObjectId{object_id(static_cast<IUnknown*>(*out))};}return wrapped;}
 HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override{auto buffer=current_buffer();auto begin=qpc_now();auto hr=native_->Present(sync,flags);auto native_end=qpc_now();after_present(hr,flags,buffer);present_metric(id_.value,begin,native_end,hr,flags);return hr;}
 HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override{auto buffer=current_buffer();auto begin=qpc_now();auto hr=native_->Present1(sync,flags,params);auto native_end=qpc_now();after_present(hr,flags,buffer);present_metric(id_.value,begin,native_end,hr,flags);return hr;}
 HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) override{auto hr=native_->ResizeBuffers(count,width,height,format,flags);if(SUCCEEDED(hr))resized();return hr;}
 HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,IUnknown*const* queues) override {
  if(!queues){auto hr=native_->ResizeBuffers1(count,width,height,format,flags,masks,nullptr);if(SUCCEEDED(hr))resized();return hr;}
  DXGI_SWAP_CHAIN_DESC1 desc{};if(!count&&FAILED(native_->GetDesc1(&desc)))return E_FAIL;UINT actual=count?count:desc.BufferCount;
  std::vector<IUnknown*> raw(actual);for(UINT i=0;i<actual;++i)raw[i]=unwrap_unknown(queues[i]);
  auto hr=native_->ResizeBuffers1(count,width,height,format,flags,masks,raw.data());if(SUCCEEDED(hr)){resized();queue_uncertain_=true;}return hr;
 }
#include "dxgi_swapchain_forward.inl"
};
HRESULT wrap_swapchain(IUnknown* native,IUnknown* parent,IUnknown* queue,REFIID iid,void** out){
 if(!out){if(native)native->Release();return E_POINTER;}*out=nullptr;ComPtr<IDXGISwapChain4> typed;auto hr=native->QueryInterface(IID_PPV_ARGS(&typed));native->Release();if(FAILED(hr)){runtime().unsupported({},"IDXGISwapChain4 native unavailable");return hr;}
 auto proxy=new(std::nothrow) Swapchain(typed.Get(),parent,queue);if(!proxy)return E_OUTOFMEMORY;hr=proxy->QueryInterface(iid,out);proxy->Release();return hr;
}
class Factory final : public IDXGIFactory7 {
 ComPtr<IDXGIFactory7> native_;std::atomic<ULONG> refs_{1};
public:
 explicit Factory(IDXGIFactory7* p):native_(p){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIFactory)||iid==__uuidof(IDXGIFactory1)||iid==__uuidof(IDXGIFactory2)||iid==__uuidof(IDXGIFactory3)||iid==__uuidof(IDXGIFactory4)||iid==__uuidof(IDXGIFactory5)||iid==__uuidof(IDXGIFactory6)||iid==__uuidof(IDXGIFactory7)){void* check{};auto hr=native_->QueryInterface(iid,&check);if(FAILED(hr))return hr;static_cast<IUnknown*>(check)->Release();*out=static_cast<IDXGIFactory7*>(this);AddRef();return S_OK;}
  runtime().unsupported({},"DXGI factory QueryInterface unsupported");return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release() override{auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name,const IUnknown* value) override{return native_->SetPrivateDataInterface(name,value);}
 HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out) override {
  if(!out)return native_->CreateSwapChain(unwrap_unknown(device),desc,nullptr);*out=nullptr;IDXGISwapChain* raw{};auto hr=native_->CreateSwapChain(unwrap_unknown(device),desc,&raw);if(FAILED(hr))return hr;return wrap_swapchain(raw,this,device,__uuidof(IDXGISwapChain),reinterpret_cast<void**>(out));
 }
 HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* restrict,IDXGISwapChain1** out) override {
  if(!out)return native_->CreateSwapChainForHwnd(unwrap_unknown(device),hwnd,desc,full,restrict,nullptr);*out=nullptr;IDXGISwapChain1* raw{};auto hr=native_->CreateSwapChainForHwnd(unwrap_unknown(device),hwnd,desc,full,restrict,&raw);if(FAILED(hr))return hr;return wrap_swapchain(raw,this,device,__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(out));
 }
 HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device,IUnknown* window,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* restrict,IDXGISwapChain1** out) override {
  if(!out)return native_->CreateSwapChainForCoreWindow(unwrap_unknown(device),window,desc,restrict,nullptr);*out=nullptr;IDXGISwapChain1* raw{};auto hr=native_->CreateSwapChainForCoreWindow(unwrap_unknown(device),window,desc,restrict,&raw);if(FAILED(hr))return hr;return wrap_swapchain(raw,this,device,__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(out));
 }
 HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* restrict,IDXGISwapChain1** out) override {
  if(!out)return native_->CreateSwapChainForComposition(unwrap_unknown(device),desc,restrict,nullptr);*out=nullptr;IDXGISwapChain1* raw{};auto hr=native_->CreateSwapChainForComposition(unwrap_unknown(device),desc,restrict,&raw);if(FAILED(hr))return hr;return wrap_swapchain(raw,this,device,__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(out));
 }
#include "dxgi_factory_forward.inl"
};
HRESULT create_factory(UINT flags,REFIID iid,void** out,int version){
 WCHAR system[MAX_PATH];if(!GetSystemDirectoryW(system,MAX_PATH))return E_FAIL;
 static HMODULE module=LoadLibraryExW((std::wstring(system)+L"\\dxgi.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)return HRESULT_FROM_WIN32(GetLastError());
 IUnknown* raw{};HRESULT hr;
 if(version==2){auto proc=reinterpret_cast<decltype(&CreateDXGIFactory2)>(GetProcAddress(module,"CreateDXGIFactory2"));hr=proc(flags,iid,out?reinterpret_cast<void**>(&raw):nullptr);}
 else{auto proc=reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(module,version?"CreateDXGIFactory1":"CreateDXGIFactory"));hr=proc(iid,out?reinterpret_cast<void**>(&raw):nullptr);}
 if(FAILED(hr)||!out)return hr;*out=nullptr;ComPtr<IDXGIFactory7> typed;auto qi=raw->QueryInterface(IID_PPV_ARGS(&typed));raw->Release();if(FAILED(qi))return qi;
 auto proxy=new(std::nothrow) Factory(typed.Get());if(!proxy)return E_OUTOFMEMORY;auto result=proxy->QueryInterface(iid,out);proxy->Release();return FAILED(result)?result:hr;
}
}
}
extern "C" __declspec(dllexport) HRESULT WINAPI Arc2CreateDXGIFactory(REFIID iid,void** out){return arc::arc2::create_factory(0,iid,out,0);}
extern "C" __declspec(dllexport) HRESULT WINAPI Arc2CreateDXGIFactory1(REFIID iid,void** out){return arc::arc2::create_factory(0,iid,out,1);}
extern "C" __declspec(dllexport) HRESULT WINAPI Arc2CreateDXGIFactory2(UINT flags,REFIID iid,void** out){return arc::arc2::create_factory(flags,iid,out,2);}
extern "C" unsigned long long WINAPI Arc2AcceptedActions();
extern "C" unsigned long long WINAPI Arc2RejectedActions();
extern "C" __declspec(dllexport) HRESULT WINAPI Arc2Dump(const wchar_t* path){try{if(!path)return E_POINTER;arc::arc2::stop_observer();arc::arc2::save_observer(path);arc::arc2::save_present_metrics(path);std::ofstream file(std::filesystem::path(path),std::ios::binary);auto data=arc::arc2::serialize(arc::arc2::runtime().snapshot());data.pop_back();file<<data<<",\"accepted_actions\":"<<Arc2AcceptedActions()<<",\"rejected_actions\":"<<Arc2RejectedActions()<<'}';return file?S_OK:E_FAIL;}catch(...){return E_FAIL;}}
