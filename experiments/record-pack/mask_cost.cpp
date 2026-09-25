// CPU-only whole-batch replay of mask expansion. Upload-memory destinations.
// Inputs before the mask loop are assumed available: this is an optimistic scope.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
using Microsoft::WRL::ComPtr;
struct Input {uint32_t r[4];float transparency,fade,radius,alpha;uint32_t lod,stencil,pad0,pad1;};
struct Task {uint32_t base,mask,offset,reserved;};
static_assert(sizeof(Input)==48 && sizeof(Task)==16);
using Clock=std::chrono::steady_clock;
void ck(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 error");}
float half(uint16_t h){int e=(h>>10)&31,m=h&1023;if(e==31)throw std::runtime_error("nonfinite distance");return (h&32768?-1.f:1.f)*std::ldexp(float(m+(e?1024:0)),e?e-25:-24);}
__declspec(noinline) uint32_t original(const Task* src,uint32_t n,uint32_t* dst){
 uint32_t count=0;
 for(uint32_t i=0;i<n;++i){uint32_t m=src[i].mask;while(m){uint32_t bit=std::countr_zero(m);m^=1u<<bit;dst[count++]=src[i].base|(bit<<24);}}
 _mm_sfence();return count;
}
__declspec(noinline) uint32_t prepare(const Task* src,uint32_t n,Task* dst){
 uint32_t count=0;
 for(uint32_t i=0;i<n;++i){dst[i]={src[i].base,src[i].mask,count,0};count+=std::popcount(src[i].mask);}
 _mm_sfence();return count;
}
int wmain(int argc,wchar_t** argv){try{
 if(argc!=4)return 2;
 std::ifstream f(argv[1],std::ios::binary|std::ios::ate);auto size=f.tellg();if(size<=0||size>8*1024*1024||size%48)throw std::runtime_error("input bounds");f.seekg(0);
 std::vector<Input> input(size_t(size)/48);f.read((char*)input.data(),size);
 std::vector<Task> tasks;for(auto v:input){if(!std::isfinite(v.radius)||v.radius<=0)throw std::runtime_error("radius");float d=std::max(v.transparency,std::max(0.f,half(uint16_t(v.r[2]))-v.fade)/v.radius);tasks.push_back({(v.r[1]&0xffffff)|((uint32_t(d*15)&15)<<28),d>.99f?0u:v.r[2]>>16,0,0});}
 uint32_t n=uint32_t(tasks.size());std::vector<uint32_t> reference(n*16);auto words=original(tasks.data(),n,reference.data());reference.resize(words);
 std::ifstream e(argv[2],std::ios::binary|std::ios::ate);if(size_t(e.tellg())!=words*4)throw std::runtime_error("expected size");e.seekg(0);std::vector<uint32_t> expected(words);e.read((char*)expected.data(),words*4);if(expected!=reference)throw std::runtime_error("original output mismatch");
 ComPtr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<ID3D12Device> device;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))break;}
 if(!device)throw std::runtime_error("no hardware device");
 auto upload=[&](uint64_t bytes){D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;ck(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&r)));return r;};
 auto output=upload(uint64_t(n)*64),packet=upload(uint64_t(n)*16);uint32_t* out{};Task* packed{};D3D12_RANGE empty{};ck(output->Map(0,&empty,(void**)&out));ck(packet->Map(0,&empty,(void**)&packed));
 std::ofstream csv(argv[3]);csv<<"iteration,mode,records,words,ns,input_bytes,output_bytes\n";
 for(int i=-3;i<20;++i){auto run=[&](bool prep){auto start=Clock::now();auto count=prep?prepare(tasks.data(),n,packed):original(tasks.data(),n,out);auto end=Clock::now();if(count!=words)throw std::runtime_error("count mismatch");csv<<i<<','<<(prep?"packet_prepare":"mask_original")<<','<<n<<','<<count<<','<<std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count()<<','<<n*16<<','<<(prep?n*16:words*4)<<'\n';};if(i&1){run(true);run(false);}else{run(false);run(true);}}
 if(std::memcmp(out,expected.data(),words*4))throw std::runtime_error("mapped output mismatch");
 uint32_t count=0;for(uint32_t i=0;i<n;++i){Task p=packed[i];if(p.base!=tasks[i].base||p.mask!=tasks[i].mask||p.offset!=count)throw std::runtime_error("packet mismatch");count+=std::popcount(p.mask);}
 std::cout<<"records="<<n<<" words="<<words<<" exact=1 allocations_during_timing=0 gpu_dispatches=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
