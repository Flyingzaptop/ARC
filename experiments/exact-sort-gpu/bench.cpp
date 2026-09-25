// Standalone economic experiment. Own buffers only; no application hooks.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
void check(HRESULT h){if(FAILED(h)){std::ostringstream s;s<<"HRESULT "<<std::hex<<uint32_t(h);throw std::runtime_error(s.str());}}
std::vector<char> read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("open input");auto n=f.tellg();if(n<0||n>8*1024*1024)throw std::runtime_error("file budget");std::vector<char>b(size_t(n),0);f.seekg(0);f.read(b.data(),n);if(!f)throw std::runtime_error("read input");return b;}
std::string sha(const std::vector<char>& b){BCRYPT_ALG_HANDLE a{};BCRYPT_HASH_HANDLE h{};if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash provider");DWORD n{},got{};BCryptGetProperty(a,BCRYPT_OBJECT_LENGTH,(PUCHAR)&n,4,&got,0);std::vector<UCHAR>o(n);UCHAR digest[32]{};if(BCryptCreateHash(a,&h,o.data(),n,nullptr,0,0)<0)throw std::runtime_error("hash create");BCryptHashData(h,(PUCHAR)b.data(),ULONG(b.size()),0);BCryptFinishHash(h,digest,32,0);BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);std::ostringstream s;for(auto v:digest)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();}
struct Native {
 void* memory{};using Fn=void(*)(void*,void*,int64_t,uint64_t);Fn fn{};
 explicit Native(const std::vector<char>& b){
  if(sha(b)!="d40e3fa146ee51711c31d7b87b12e9e42f8a862bc063314e1e30257c4d07fe1a")throw std::runtime_error("unreviewed original code");
  struct Part{uint32_t r,n;const char* p;};std::vector<Part>v;uint32_t count{};memcpy(&count,b.data(),4);size_t o=4;uint32_t lo=~0u,hi=0;
  for(uint32_t i=0;i<count;++i){uint32_t r,n;memcpy(&r,b.data()+o,4);memcpy(&n,b.data()+o+4,4);o+=8;v.push_back({r,n,b.data()+o});o+=n;lo=std::min(lo,r);hi=std::max(hi,r+n);}
  memory=VirtualAlloc(nullptr,hi-lo,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!memory)throw std::runtime_error("code allocation");
  for(auto p:v)memcpy((char*)memory+p.r-lo,p.p,p.n);DWORD old{};if(!VirtualProtect(memory,hi-lo,PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),memory,hi-lo))throw std::runtime_error("code protect");fn=(Fn)((char*)memory+v[0].r-lo);
 }
 ~Native(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
};
struct Gpu {
 ComPtr<ID3D12Device> d;ComPtr<ID3D12CommandQueue> q;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> cmd;
 ComPtr<ID3D12Fence> fence;HANDLE event{};uint64_t fenceValue{},frequency{};
 ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> init,work,advance;ComPtr<ID3D12CommandSignature> indirect;
 ComPtr<ID3D12Resource> data,a,b,counts,args,upload,readback,timeback;ComPtr<ID3D12QueryHeap> timestamps;
 char* uploadPtr{};char* readPtr{};uint64_t* timePtr{};uint32_t n{},bytes{},rounds{},budget{};bool used{};
 ComPtr<ID3D12Resource> buffer(uint64_t size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,bool uav=false){
  D3D12_HEAP_PROPERTIES h{};h.Type=type;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=size;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;r.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;ComPtr<ID3D12Resource>x;check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&x)));return x;
 }
 ComPtr<ID3D12PipelineState> pipeline(const std::filesystem::path& path,const char* entry){ComPtr<ID3DBlob>code,error;auto hr=D3DCompileFromFile(path.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)std::cerr<<(char*)error->GetBufferPointer();check(hr);D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={code->GetBufferPointer(),code->GetBufferSize()};ComPtr<ID3D12PipelineState>x;check(d->CreateComputePipelineState(&p,IID_PPV_ARGS(&x)));return x;}
 Gpu(uint32_t count,uint32_t ideal,const std::filesystem::path& shader):n(count),bytes(count*16),budget(ideal){
  if(GetEnvironmentVariableW(L"ARC_SORT_DEBUG",nullptr,0)){ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();}
  ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>x;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&x))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};x->GetDesc1(&desc);if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(x.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)))){adapter=x;std::wcout<<L"Adapter: "<<desc.Description<<L"\n";break;}}
  if(!d)throw std::runtime_error("no hardware D3D12 device");
  D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));check(q->GetTimestampFrequency(&frequency));check(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&allocator)));check(d->CreateCommandList(0,qd.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));check(cmd->Close());check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("event");
  D3D12_ROOT_PARAMETER p[6]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants={0,0,4};for(int i=1;i<6;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i-1;}
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=6;rd.pParameters=p;ComPtr<ID3DBlob>rb,re;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&rb,&re));check(d->CreateRootSignature(0,rb->GetBufferPointer(),rb->GetBufferSize(),IID_PPV_ARGS(&root)));
  init=pipeline(shader,"init");work=pipeline(shader,"work");advance=pipeline(shader,"advance");
  D3D12_INDIRECT_ARGUMENT_DESC id{};id.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC sd{};sd.ByteStride=12;sd.NumArgumentDescs=1;sd.pArgumentDescs=&id;check(d->CreateCommandSignature(&sd,nullptr,IID_PPV_ARGS(&indirect)));
  data=buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,true);a=buffer(n*12,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);b=buffer(n*12,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);counts=buffer(16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);args=buffer(12,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
  upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);readback=buffer(bytes+16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);timeback=buffer(48,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_RANGE empty{};check(upload->Map(0,&empty,(void**)&uploadPtr));check(readback->Map(0,nullptr,(void**)&readPtr));check(timeback->Map(0,nullptr,(void**)&timePtr));D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=6;check(d->CreateQueryHeap(&hd,IID_PPV_ARGS(&timestamps)));
  uint32_t v=budget;rounds=2;while(v){v=(v>>1)+(v>>2);++rounds;}
  std::cout<<"records="<<n<<" rounds="<<rounds<<" timestamp_hz="<<frequency<<" explicit_buffer_bytes="<<(uint64_t(bytes)*3+uint64_t(n)*24+92)<<"\n";
 }
 ~Gpu(){if(event)CloseHandle(event);}
 void transition(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};cmd->ResourceBarrier(1,&b);}
 void barrier(){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;cmd->ResourceBarrier(1,&b);}
 void stamp(UINT i){cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i);}
 void run(const std::vector<char>& input,const std::vector<char>& expected,std::vector<char>& result,std::ostream& out,int iteration,bool warm){
  auto t0=Clock::now();memcpy(uploadPtr,input.data(),bytes);auto t1=Clock::now();
  check(allocator->Reset());check(cmd->Reset(allocator.Get(),init.Get()));cmd->SetComputeRootSignature(root.Get());ID3D12Resource* resources[]={data.Get(),a.Get(),b.Get(),counts.Get(),args.Get()};for(UINT i=0;i<5;++i)cmd->SetComputeRootUnorderedAccessView(1+i,resources[i]->GetGPUVirtualAddress());uint32_t params[]={n,0,budget,n};cmd->SetComputeRoot32BitConstants(0,4,params,0);
  stamp(0);if(used){transition(data.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);transition(args.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}cmd->CopyBufferRegion(data.Get(),0,upload.Get(),0,bytes);transition(data.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);stamp(1);
  cmd->Dispatch(1,1,1);barrier();transition(args.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);stamp(2);
  for(UINT depth=0;depth<rounds;++depth){params[1]=depth&1;cmd->SetComputeRoot32BitConstants(0,4,params,0);cmd->SetPipelineState(work.Get());cmd->ExecuteIndirect(indirect.Get(),1,args.Get(),0,nullptr,0);barrier();if(depth==0)stamp(5);transition(args.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cmd->SetPipelineState(advance.Get());cmd->Dispatch(1,1,1);barrier();transition(args.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);}
  stamp(3);transition(data.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(counts.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(readback.Get(),0,data.Get(),0,bytes);cmd->CopyBufferRegion(readback.Get(),bytes,counts.Get(),0,16);transition(counts.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);stamp(4);cmd->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,6,timeback.Get(),0);check(cmd->Close());auto t2=Clock::now();
  ID3D12CommandList* list=cmd.Get();q->ExecuteCommandLists(1,&list);check(q->Signal(fence.Get(),++fenceValue));auto t3=Clock::now();if(fence->GetCompletedValue()<fenceValue){check(fence->SetEventOnCompletion(fenceValue,event));if(WaitForSingleObject(event,15000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout");}auto t4=Clock::now();check(d->GetDeviceRemovedReason());memcpy(result.data(),readPtr,bytes);auto t5=Clock::now();used=true;
  uint32_t status[4];memcpy(status,readPtr+bytes,16);uint64_t errors=0;for(size_t p=0;p<bytes;p+=16)errors+=memcmp(result.data()+p,expected.data()+p,16)!=0;
  auto gpu=[&](int a,int b){return double(timePtr[b]-timePtr[a])*1000/double(frequency);};
  out<<"gpu,"<<iteration<<','<<warm<<','<<ms(t0,t1)<<','<<ms(t1,t2)<<','<<ms(t2,t3)<<','<<ms(t3,t4)<<','<<ms(t4,t5)<<','<<ms(t0,t5)<<','<<gpu(0,1)<<','<<gpu(1,2)<<','<<gpu(2,3)<<','<<gpu(3,4)<<','<<gpu(2,5)<<','<<errors<<','<<status[0]+status[1]<<','<<status[2]<<'\n';out.flush();
  ComPtr<ID3D12InfoQueue> info;
  if(SUCCEEDED(d.As(&info))){for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char>msg(size);auto m=reinterpret_cast<D3D12_MESSAGE*>(msg.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<'\n';throw std::runtime_error("D3D12 validation error");}}info->ClearStoredMessages();}
  if(errors||status[0]||status[1]||status[2])throw std::runtime_error("GPU exact-result/queue validation failed");
 }
};
int wmain(int argc,wchar_t** argv){try{
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);if(argc<7){std::cerr<<"input expected closure shader csv repetitions [ideal]\n";return 2;}auto started=Clock::now();auto input=read(argv[1]),expected=read(argv[2]);if(input.size()!=expected.size()||input.size()%16||input.size()<32||input.size()>4*1024*1024)throw std::runtime_error("array bounds");uint32_t n=uint32_t(input.size()/16),budget=argc>7?std::stoul(argv[7]):n;if(budget>n)throw std::runtime_error("budget");Native native(read(argv[3]));Gpu gpu(n,budget,argv[4]);std::cout<<"initialization_ms="<<ms(started,Clock::now())<<" input_sha256="<<sha(input)<<"\n";
 std::ofstream out(argv[5]);out<<std::setprecision(10)<<"mode,iteration,warmup,prep_ms,record_ms,submit_ms,wait_ms,consume_ms,full_ms,gpu_upload_ms,gpu_init_ms,gpu_sort_ms,gpu_return_ms,gpu_root_ms,mismatch_records,pending_tasks,gpu_error\n";
 // Keep native array alignment independent of the standard allocator.
 void* aligned=_aligned_malloc(input.size(),16);if(!aligned)throw std::bad_alloc();std::vector<char>result(input.size());int reps=std::stoi(argv[6]);if(reps<1||reps>30)throw std::runtime_error("repetition budget");
 auto cpu=[&](int i){memcpy(aligned,input.data(),input.size());auto t=Clock::now();native.fn(aligned,(char*)aligned+input.size(),budget,0);auto end=Clock::now();if(memcmp(aligned,expected.data(),input.size()))throw std::runtime_error("original CPU reference mismatch");out<<"cpu,"<<i<<','<<(i<0)<<",0,0,0,0,0,"<<ms(t,end)<<",0,0,0,0,0,0,0,0\n";out.flush();};
 for(int i=-1;i<reps;++i){if((i&1)==0){cpu(i);gpu.run(input,expected,result,out,i,i<0);}else{gpu.run(input,expected,result,out,i,i<0);cpu(i);}}
 _aligned_free(aligned);return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
