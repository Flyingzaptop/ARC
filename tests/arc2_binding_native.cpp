#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "arc/arc2/bootstrap.hpp"
using Microsoft::WRL::ComPtr;
static void ok(HRESULT h){if(FAILED(h))throw std::runtime_error("HRESULT "+std::to_string(unsigned(h)));}
template<class T,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind> struct alignas(void*) StreamPart {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind=Kind;T value{};};
struct MixedStream {StreamPart<UINT,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK> node;StreamPart<ID3D12RootSignature*,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;StreamPart<D3D12_SHADER_BYTECODE,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> cs;StreamPart<D3D12_PIPELINE_STATE_FLAGS,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS> flags;};
int main(){try{
 HMODULE legacy{};wchar_t dll[32768]{},path[32768]{};
 if(GetEnvironmentVariableW(L"ARC1_DLL",dll,32768)){legacy=LoadLibraryW(dll);if(!legacy)throw std::runtime_error("legacy module");GetEnvironmentVariableW(L"ARC1_OUTPUT",path,32768);auto init=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcInitialize"));auto begin=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcBeginCapture"));SetEnvironmentVariableW(L"ARC_AUTO_CONFIG",nullptr);if(!init||init(path)||!begin||begin(nullptr))throw std::runtime_error("legacy capture setup");}

 ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> q;ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
 ComPtr<ID3D12CommandAllocator> allocator;ok(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
 ComPtr<ID3D12GraphicsCommandList> list;ok(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
 D3D12_ROOT_PARAMETER parameters[3]{};for(int i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i]};}parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants={0,0,1};
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=parameters;ComPtr<ID3DBlob> blob,error;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,blob.GetAddressOf(),&error));
 ComPtr<ID3D12RootSignature> root;ok(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
 const char* hlsl="Buffer<uint> A:register(t0);Buffer<uint>B:register(t1);RWBuffer<uint>O:register(u0);cbuffer C:register(b0){uint bias;}[numthreads(4,1,1)]void main(uint3 id:SV_DispatchThreadID){O[id.x]=A[id.x]+B[id.x]+bias;}";
 ComPtr<ID3DBlob> shader;ok(D3DCompile(hlsl,std::strlen(hlsl),nullptr,nullptr,nullptr,"main","cs_5_1",0,0,&shader,&error));D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};ComPtr<ID3D12PipelineState> pso;ok(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
 MixedStream mixed;mixed.root.value=root.Get();mixed.cs.value=pd.CS;static_assert(offsetof(decltype(mixed.node),value)==4);static_assert(offsetof(decltype(mixed.root),value)==8);D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(mixed),&mixed};ComPtr<ID3D12Device2> d2;ok(d.As(&d2));ComPtr<ID3D12PipelineState> streamed;ok(d2->CreatePipelineState(&stream,IID_PPV_ARGS(&streamed)));

 auto resource=[&](D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=16;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;b.Flags=flags;ComPtr<ID3D12Resource> value;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,state,nullptr,IID_PPV_ARGS(&value)));return value;};
 auto a=resource(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE),b=resource(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE),output=resource(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),readback=resource(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
 for(auto r:{a.Get(),b.Get()}){unsigned* v{};ok(r->Map(0,nullptr,reinterpret_cast<void**>(&v)));for(unsigned i=0;i<4;++i)v[i]=i+1;r->Unmap(0,nullptr);}
 ComPtr<ID3D12Device> parent;ok(a->GetDevice(IID_PPV_ARGS(&parent)));if(parent.Get()!=d.Get())throw std::runtime_error("GetDevice identity");
 const GUID private_key{0x729da8b1,0x3c13,0x499e,{0x91,0x73,0x33,0xf0,0x55,0x61,0x38,0x27}};
 auto opaque=resource(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);IUnknown* original=opaque.Get();ok(q->SetPrivateDataInterface(private_key,original));opaque.Reset();IUnknown* returned{};UINT private_size=sizeof(returned);ok(q->GetPrivateData(private_key,&private_size,&returned));if(returned!=original)throw std::runtime_error("private interface identity changed");returned->Release();unsigned scalar=1234;ok(q->SetPrivateData(private_key,sizeof(scalar),&scalar));unsigned restored{};private_size=sizeof(restored);ok(q->GetPrivateData(private_key,&private_size,&restored));if(restored!=scalar)throw std::runtime_error("private scalar overwritten");

 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> heap;ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));auto cpu=heap->GetCPUDescriptorHandleForHeapStart();auto gpu=heap->GetGPUDescriptorHandleForHeapStart();auto step=d->GetDescriptorHandleIncrementSize(hd.Type);
 D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=4;d->CreateShaderResourceView(a.Get(),&srv,cpu);d->CreateShaderResourceView(b.Get(),&srv,{cpu.ptr+step});D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_UINT;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=4;d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,{cpu.ptr+2*step});
 list->SetPipelineState(pso.Get());list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* hs[]={heap.Get()};list->SetDescriptorHeaps(1,hs);list->SetComputeRootDescriptorTable(0,gpu);list->SetComputeRootDescriptorTable(1,{gpu.ptr+2*step});list->SetComputeRoot32BitConstant(2,17,0);list->Dispatch(1,1,1);
 D3D12_RESOURCE_BARRIER ub{};ub.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;ub.UAV.pResource=output.Get();list->ResourceBarrier(1,&ub);
 list->SetPipelineState(streamed.Get());list->SetComputeRootSignature(root.Get());list->SetComputeRoot32BitConstant(2,49,0);list->Dispatch(1,1,1);
 D3D12_RESOURCE_BARRIER transition{};transition.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&transition);list->CopyResource(readback.Get(),output.Get());ok(list->Close());ID3D12CommandList* cls[]={list.Get()};q->ExecuteCommandLists(1,cls);ComPtr<ID3D12Fence> f;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);ok(q->Signal(f.Get(),1));ok(f->SetEventOnCompletion(1,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("timeout");CloseHandle(event);
 unsigned* data{};ok(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)));for(unsigned i=0;i<4;++i)if(data[i]!=2*(i+1)+49)throw std::runtime_error("GPU binding result mismatch");readback->Unmap(0,nullptr);ok(d->GetDeviceRemovedReason());if(legacy){auto end=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(legacy,"ArcEndCapture"));std::wstring graph=std::wstring(path)+L".graph.json";if(!end||end(graph.data()))throw std::runtime_error("legacy capture output");}
 if(arc2_bootstrap::loader().module){
  ok(arc2_bootstrap::flush());
  wchar_t ir_path[32768]{};auto length=GetEnvironmentVariableW(L"ARC2_OUTPUT",ir_path,32768);
  if(!length||length>=32768)throw std::runtime_error("ARC2 output path missing");
  std::ifstream input(std::filesystem::path(ir_path),std::ios::binary);
  if(!input)throw std::runtime_error("ARC2 IR dump missing");
  const std::string ir{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  const std::string dispatch="\"arguments\":{\"known\":true,\"type\":\"dispatch\",\"x\":1,\"y\":1,\"z\":1}";
  const auto first=ir.find(dispatch);
  if(ir.find("\"command_payload_version\":1")==std::string::npos||first==std::string::npos||
     ir.find(dispatch,first+dispatch.size())==std::string::npos)
      throw std::runtime_error("ARC2 did not retain both exact dispatch payloads");
 }
 std::cout<<"PASS root tables/constants, same root signature, GPU readback, parent identity\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
