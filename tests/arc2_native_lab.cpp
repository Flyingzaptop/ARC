#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include "arc/arc2/bootstrap.hpp"
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("HRESULT "+std::to_string(static_cast<unsigned>(hr)));}
static double ms(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static D3D12_RESOURCE_DESC buffer(UINT64 size){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;}
static LRESULT CALLBACK wnd(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
int main(int argc,char**argv){
 try{
  const std::string prefix=argc>1?argv[1]:"arc2-lab";unsigned repetitions=argc>2?std::stoul(argv[2]):64;
  unsigned frames=argc>3?std::stoul(argv[3]):40;const bool debug=argc>4&&std::string(argv[4])=="debug";const bool toggle=argc>5&&std::string(argv[5])=="toggle";
  if(repetitions>2048||frames>1000)throw std::runtime_error("bounded lab limits");
  if(debug){ComPtr<ID3D12Debug> d;check(D3D12GetDebugInterface(IID_PPV_ARGS(&d)));d->EnableDebugLayer();}
  HMODULE legacy{};wchar_t legacy_path[32768]{};
  if(GetEnvironmentVariableW(L"ARC1_DLL",legacy_path,32768)){legacy=LoadLibraryW(legacy_path);if(!legacy)throw std::runtime_error("legacy DLL unavailable");auto initialize=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcInitialize"));auto path=std::filesystem::absolute(prefix+".legacy.json").wstring();SetEnvironmentVariableW(L"ARC_AUTO_CONFIG",nullptr);if(!initialize||initialize(path.data()))throw std::runtime_error("legacy initialize");auto begin=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcRequestFrame"));auto framepath=std::filesystem::absolute(prefix+".legacy.frame.json").wstring();if(!begin||begin(framepath.data()))throw std::runtime_error("legacy frame request");}
  WNDCLASSW wc{};wc.lpfnWndProc=wnd;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName=L"ARC2 Owned Lab";RegisterClassW(&wc);
  HWND window=CreateWindowW(wc.lpszClassName,L"ARC2 Owned Lab",WS_OVERLAPPEDWINDOW,20,20,360,260,nullptr,nullptr,wc.hInstance,nullptr);if(!window)throw std::runtime_error("window creation");
  ShowWindow(window,SW_SHOWNOACTIVATE);
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i){DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.VendorId==0x10de&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),nullptr)))break;adapter.Reset();}
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
  ComPtr<IUnknown> a,b;check(device.As(&a));check(device.As(&b));if(a.Get()!=b.Get())throw std::runtime_error("COM identity");a.Reset();b.Reset();
  D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
  DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=320;sd.Height=180;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> initial;check(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&initial));ComPtr<IDXGISwapChain3> swap;check(initial.As(&swap));initial.Reset();
  ComPtr<IDXGIFactory4> parent;check(swap->GetParent(IID_PPV_ARGS(&parent)));ComPtr<IUnknown> p0,p1;check(parent.As(&p0));check(factory.As(&p1));if(p0.Get()!=p1.Get())throw std::runtime_error("DXGI parent identity");p0.Reset();p1.Reset();parent.Reset();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;ComPtr<ID3D12DescriptorHeap> heap;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));auto first=heap->GetCPUDescriptorHandleForHeapStart();auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  ComPtr<ID3D12Resource> back[2];auto buffers=[&]{for(UINT i=0;i<2;++i){check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])));D3D12_CPU_DESCRIPTOR_HANDLE h{first.ptr+i*stride};device->CreateRenderTargetView(back[i].Get(),nullptr,h);}};buffers();
  ComPtr<ID3D12CommandAllocator> alloc;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
  ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;auto wait=[&]{check(queue->Signal(fence.Get(),++serial));if(fence->GetCompletedValue()<serial){check(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("fence timeout");}};
  D3D12_QUERY_HEAP_DESC queryd{};queryd.Count=2;queryd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;ComPtr<ID3D12QueryHeap> queries;check(device->CreateQueryHeap(&queryd,IID_PPV_ARGS(&queries)));UINT64 frequency{};check(queue->GetTimestampFrequency(&frequency));
  D3D12_HEAP_PROPERTIES rh{};rh.Type=D3D12_HEAP_TYPE_READBACK;auto rb=buffer(256*1024);ComPtr<ID3D12Resource> pixels,times;check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&pixels)));auto tb=buffer(16);check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&tb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&times)));
  std::ofstream csv(prefix+".csv");csv<<"frame,warmup,cpu_submission_ms,present_return_ms,frame_ms,gpu_ms,successful_present\n";
  double previous=ms();
  for(unsigned frame=0;frame<frames;++frame){MSG msg;while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}
   if(frame==frames/2){if(toggle){auto fn=reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(arc2_bootstrap::loader().module,"Arc2SetOptimizationEnabled"));if(!fn)throw std::runtime_error("rollback control unavailable");fn(FALSE);}for(auto& value:back)value.Reset();check(swap->ResizeBuffers(2,320,180,DXGI_FORMAT_R8G8B8A8_UNORM,0));buffers();}
   auto start=ms();check(alloc->Reset());check(list->Reset(alloc.Get(),nullptr));auto index=swap->GetCurrentBackBufferIndex();auto resource=back[index].Get();
   D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};list->ResourceBarrier(1,&barrier);
   list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
   const FLOAT color[4]={0.125f,0.5f,0.75f,1.0f};D3D12_CPU_DESCRIPTOR_HANDLE target{first.ptr+index*stride};
   for(unsigned i=0;i<repetitions;++i)list->ClearRenderTargetView(target,color,0,nullptr);
   list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
   if(frame+1==frames){barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&barrier);D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=resource;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=pixels.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,320,180,1,1280};list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;}else barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
   barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;list->ResourceBarrier(1,&barrier);check(list->Close());ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);auto submitted=ms();auto phr=swap->Present(0,0);auto presented=ms();check(phr);wait();
   UINT64* mapped{};D3D12_RANGE read{0,16};check(times->Map(0,&read,reinterpret_cast<void**>(&mapped)));double gpu=double(mapped[1]-mapped[0])*1000/double(frequency);D3D12_RANGE none{0,0};times->Unmap(0,&none);
   csv<<frame<<','<<(frame<8)<<','<<submitted-start<<','<<presented-submitted<<','<<presented-previous<<','<<gpu<<','<<(phr==S_OK)<<'\n';previous=presented;
  }
  void* mapped{};D3D12_RANGE read{0,320*180*4};check(pixels->Map(0,&read,&mapped));std::ofstream image(prefix+".rgba8",std::ios::binary);image.write(static_cast<char*>(mapped),read.End);D3D12_RANGE none{0,0};pixels->Unmap(0,&none);
  wait();
  if(debug&&arc2_bootstrap::loader().module){auto diagnostic=reinterpret_cast<unsigned long long(WINAPI*)(IUnknown*,const wchar_t*)>(GetProcAddress(arc2_bootstrap::loader().module,"Arc2DebugErrors"));if(!diagnostic)throw std::runtime_error("missing debug diagnostic");auto errors=diagnostic(device.Get(),std::filesystem::path(prefix+".debug.txt").c_str());if(errors)throw std::runtime_error("debug errors or unavailable queue "+std::to_string(errors));}
  if(legacy){auto end=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcEndCapture"));auto snapshot=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcSnapshot"));auto path=std::filesystem::absolute(prefix+".legacy.graph.json").wstring();if(!end||end(path.data()))throw std::runtime_error("legacy capture");if(snapshot)snapshot(nullptr);}
  if(arc2_bootstrap::loader().module){char mode[32]{};GetEnvironmentVariableA("ARC2_MODE",mode,32);if(std::string(mode)=="optimize"){auto count=reinterpret_cast<unsigned long long(WINAPI*)()>(GetProcAddress(arc2_bootstrap::loader().module,"Arc2AcceptedActions"));const auto expected=static_cast<unsigned long long>(toggle?frames/2:frames)*(repetitions?repetitions-1:0);if(!count||count()!=expected)throw std::runtime_error("rewrite/rollback count mismatch");}}
  CloseHandle(event);DestroyWindow(window);if(arc2_bootstrap::loader().module)check(arc2_bootstrap::flush());std::cout<<"{\"status\":\"PASS\",\"frames\":"<<frames<<",\"resize\":true,\"width\":320,\"height\":180,\"display_fps_measured\":false}\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
