// Isolated three-dispatch GPU prefix/scatter benchmark. Never touches a game process.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;

static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("HRESULT "+std::to_string(uint32_t(hr)));}
static double millis(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static std::vector<char> read(const wchar_t* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("open input");auto n=f.tellg();if(n<0||n>16*1024*1024)throw std::runtime_error("input size");std::vector<char> data(size_t(n),0);f.seekg(0);f.read(data.data(),n);if(!f)throw std::runtime_error("read input");return data;}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* d,uint64_t n,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,bool uav=false){D3D12_HEAP_PROPERTIES h{};h.Type=type;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=n;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;r.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;ComPtr<ID3D12Resource> out;check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&out)));return out;}
static void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);}
static ComPtr<ID3DBlob> shader(const std::wstring& path){ComPtr<ID3DBlob> code,error;auto hr=D3DCompileFromFile(path.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_1",D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);if(FAILED(hr)&&error)std::cerr<<(char*)error->GetBufferPointer()<<'\n';check(hr);return code;}
static ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* d,ID3D12RootSignature* root,ID3DBlob* code){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code->GetBufferPointer(),code->GetBufferSize()};ComPtr<ID3D12PipelineState> out;check(d->CreateComputePipelineState(&p,IID_PPV_ARGS(&out)));return out;}
static size_t compare(const char* actual,const std::vector<char>& expected,const char* label){size_t wrong=0;for(size_t i=0;i<expected.size();i+=4)if(std::memcmp(actual+i,expected.data()+i,4)){if(wrong<12){uint32_t a=0,b=0;std::memcpy(&a,actual+i,4);std::memcpy(&b,expected.data()+i,4);std::cerr<<label<<" word "<<i/4<<" "<<std::hex<<a<<" != "<<b<<std::dec<<'\n';}++wrong;}return wrong;}

int wmain(int argc,wchar_t** argv){try{
 if(argc!=10){std::cerr<<"usage: compaction-bench SHADER_DIR SCRATCH PACKED_EXPECTED METADATA_EXPECTED COUNT SCRATCH_WORDS RECORD_WORDS INITIAL_CURSOR CSV\n";return 2;}
 const std::wstring dir=argv[1];const auto scratchBytes=read(argv[2]),expectedPacked=read(argv[3]),expectedMeta=read(argv[4]);
 const uint32_t count=std::stoul(argv[5]),scratchWords=std::stoul(argv[6]),recordWords=std::stoul(argv[7]);const uint64_t initialCursor=std::stoull(argv[8]);
 if(!count||count>65536||scratchWords<3+recordWords||recordWords<1||recordWords>64||scratchBytes.size()!=uint64_t(count)*scratchWords*4||expectedMeta.size()!=16||expectedPacked.size()>uint64_t(count)*recordWords*4||expectedPacked.size()%4)throw std::runtime_error("shape mismatch");
 const uint32_t groups=(count+255)/256;
 ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<ID3D12Device> d;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d))))break;}
 if(!d)throw std::runtime_error("hardware D3D12 unavailable");
 D3D12_ROOT_PARAMETER p[7]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants={0,0,4};
 for(UINT i=0;i<3;++i){p[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1+i].Descriptor.ShaderRegister=i;}
 for(UINT i=0;i<3;++i){p[4+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[4+i].Descriptor.ShaderRegister=i;}
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=7;rd.pParameters=p;ComPtr<ID3DBlob> rootBlob,error;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,&error));ComPtr<ID3D12RootSignature> root;check(d->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)));
 auto local=pipeline(d.Get(),root.Get(),shader(dir+L"/prefix-local.hlsl").Get());auto group=pipeline(d.Get(),root.Get(),shader(dir+L"/prefix-groups.hlsl").Get());auto scatter=pipeline(d.Get(),root.Get(),shader(dir+L"/scatter.hlsl").Get());
 auto upload=buffer(d.Get(),scratchBytes.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto scratch=buffer(d.Get(),scratchBytes.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto prefix=buffer(d.Get(),uint64_t(count)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto sums=buffer(d.Get(),uint64_t(groups)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto invalids=buffer(d.Get(),uint64_t(groups)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto offsets=buffer(d.Get(),uint64_t(groups)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto packed=buffer(d.Get(),uint64_t(count)*recordWords*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto metadata=buffer(d.Get(),16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
 auto backPacked=buffer(d.Get(),uint64_t(count)*recordWords*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 auto backMeta=buffer(d.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 auto timeRead=buffer(d.Get(),6*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=6;ComPtr<ID3D12QueryHeap> query;check(d->CreateQueryHeap(&qd,IID_PPV_ARGS(&query)));
 D3D12_COMMAND_QUEUE_DESC queueDesc{};ComPtr<ID3D12CommandQueue> queue;check(d->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));UINT64 frequency{};check(queue->GetTimestampFrequency(&frequency));ComPtr<ID3D12CommandAllocator> alloc;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));ComPtr<ID3D12GraphicsCommandList> cmd;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));check(cmd->Close());ComPtr<ID3D12Fence> fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("event");
 char* up{};char* result{};char* meta{};UINT64* ticks{};D3D12_RANGE empty{};check(upload->Map(0,&empty,(void**)&up));check(backPacked->Map(0,nullptr,(void**)&result));check(backMeta->Map(0,nullptr,(void**)&meta));check(timeRead->Map(0,nullptr,(void**)&ticks));
 std::ofstream csv(argv[9]);csv<<std::setprecision(10)<<"iteration,count,prep_ms,record_ms,submit_ms,wait_ms,consume_ms,full_ms,gpu_upload_ms,gpu_local_prefix_ms,gpu_group_prefix_ms,gpu_scatter_ms,gpu_return_ms,validated,mismatch_words\n";
 UINT64 signal=0;for(int iteration=-1;iteration<20;++iteration){auto t0=Clock::now();std::memcpy(up,scratchBytes.data(),scratchBytes.size());auto t1=Clock::now();check(alloc->Reset());check(cmd->Reset(alloc.Get(),nullptr));cmd->SetComputeRootSignature(root.Get());cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
  if(iteration!=-1)transition(cmd.Get(),scratch.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  cmd->CopyBufferRegion(scratch.Get(),0,upload.Get(),0,scratchBytes.size());transition(cmd.Get(),scratch.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
  if(iteration!=-1){transition(cmd.Get(),prefix.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(cmd.Get(),sums.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(cmd.Get(),invalids.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(cmd.Get(),offsets.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(cmd.Get(),packed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(cmd.Get(),metadata.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
  uint32_t c0[4]={count,scratchWords,recordWords,0};cmd->SetPipelineState(local.Get());cmd->SetComputeRoot32BitConstants(0,4,c0,0);cmd->SetComputeRootShaderResourceView(1,scratch->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,prefix->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(5,sums->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(6,invalids->GetGPUVirtualAddress());cmd->Dispatch(groups,1,1);transition(cmd.Get(),prefix.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(cmd.Get(),sums.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(cmd.Get(),invalids.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
  uint32_t c1[4]={count,recordWords*4,uint32_t(initialCursor),uint32_t(initialCursor>>32)};cmd->SetPipelineState(group.Get());cmd->SetComputeRoot32BitConstants(0,4,c1,0);cmd->SetComputeRootShaderResourceView(1,sums->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(2,invalids->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,offsets->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(5,metadata->GetGPUVirtualAddress());cmd->Dispatch(1,1,1);transition(cmd.Get(),offsets.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);
  uint32_t c2[4]={count,scratchWords,recordWords,0};cmd->SetPipelineState(scatter.Get());cmd->SetComputeRoot32BitConstants(0,4,c2,0);cmd->SetComputeRootShaderResourceView(1,scratch->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(2,prefix->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(3,offsets->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,packed->GetGPUVirtualAddress());cmd->Dispatch(groups,1,1);cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,4);
  transition(cmd.Get(),packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(cmd.Get(),metadata.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(backPacked.Get(),0,packed.Get(),0,expectedPacked.size());cmd->CopyBufferRegion(backMeta.Get(),0,metadata.Get(),0,16);cmd->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,5);cmd->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,6,timeRead.Get(),0);check(cmd->Close());auto t2=Clock::now();ID3D12CommandList* list=cmd.Get();queue->ExecuteCommandLists(1,&list);check(queue->Signal(fence.Get(),++signal));auto t3=Clock::now();if(fence->GetCompletedValue()<signal){check(fence->SetEventOnCompletion(signal,event));if(WaitForSingleObject(event,15000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");}auto t4=Clock::now();check(d->GetDeviceRemovedReason());volatile uint32_t consumed=0;for(size_t j=0;j<expectedPacked.size();j+=4){uint32_t word;std::memcpy(&word,result+j,4);consumed^=word;}auto t5=Clock::now();(void)consumed;bool validated=iteration==-1||iteration==19;size_t wrong=validated?compare(result,expectedPacked,"record")+compare(meta,expectedMeta,"metadata"):0;auto gpu=[&](int a,int b){return double(ticks[b]-ticks[a])*1000.0/double(frequency);};csv<<iteration<<','<<count<<','<<millis(t0,t1)<<','<<millis(t1,t2)<<','<<millis(t2,t3)<<','<<millis(t3,t4)<<','<<millis(t4,t5)<<','<<millis(t0,t5)<<','<<gpu(0,1)<<','<<gpu(1,2)<<','<<gpu(2,3)<<','<<gpu(3,4)<<','<<gpu(4,5)<<','<<validated<<','<<wrong<<'\n';csv.flush();if(wrong)throw std::runtime_error("compaction output mismatch");}
 CloseHandle(event);std::cout<<"compaction_exact=1 live_replacement=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
