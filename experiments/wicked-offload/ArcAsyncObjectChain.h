#pragma once
// Source-assisted experiment. No automatic ARC admission. No frame-stale results.
#include "ArcControlledVisibility.h"
#include <array>
#include "ArcChainContract.h"

namespace arc_chain {
using namespace arc_controlled;
inline void fail(const char* reason){char path[1024]{};GetEnvironmentVariableA("ARC_WICKED_CPU_PROFILE",path,900);std::strcat(path,".chain-error.txt");FILE* f{};fopen_s(&f,path,"w");if(f){std::fprintf(f,"%s\n",reason);std::fclose(f);}ExitProcess(95);}
using AABB=wi::primitive::AABB;
using Matrix=DirectX::XMFLOAT4X4;
using Result=ArcChainResult;
static_assert(sizeof(Result)==48);
struct Slot : State {
    ComPtr<ID3D12Resource> metadata,metadataUpload;
    std::vector<AABB> cached;
    uint32_t count{},offset{},limit{};
    bool pending{};
    Clock::time_point submitted{};
    void prepare(uint32_t n){
        if(!queue){
            // Reuse the tested device/fence/allocator setup, replacing only PSO/root.
            State::init(1);
            const char* shader=R"(
cbuffer Params:register(b0){float4 planes[6];uint count;};
ByteAddressBuffer matrices:register(t0);ByteAddressBuffer geometry:register(t1);
RWStructuredBuffer<uint> result:register(u0);
groupshared uint localCount,base;groupshared uint ids[128];
[numthreads(128,1,1)] void main(uint3 tid:SV_DispatchThreadID,uint li:SV_GroupIndex){
if(li==0)localCount=0;GroupMemoryBarrierWithGroupSync();
uint i=tid.x;bool visible=false;
if(i<count){
float3 lo=asfloat(geometry.Load3(i*32));uint layer=geometry.Load(i*32+12);float3 hi=asfloat(geometry.Load3(i*32+16));
float3 r0=asfloat(matrices.Load3(i*64));float3 r1=asfloat(matrices.Load3(i*64+16));
float3 r2=asfloat(matrices.Load3(i*64+32));float3 r3=asfloat(matrices.Load3(i*64+48));
float3 low=0,high=0;
// Same eight-corner order and SSE arithmetic order as original AABB::transform.
uint patterns[8]={0,2,6,4,1,3,7,5};
[unroll]for(uint c=0;c<8;c++){
uint bits=patterns[c];float3 v=float3(bits&1?hi.x:lo.x,bits&2?hi.y:lo.y,bits&4?hi.z:lo.z);
precise float3 x=v.x*r0;precise float3 y=v.y*r1;precise float3 z=v.z*r2;
precise float3 a=x+y;precise float3 b=a+z;precise float3 p=b+r3;
if(c==0){low=p;high=p;}else{low=min(low,p);high=max(high,p);}}
precise float3 center=(low+high)*0.5;
visible=layer!=0 && !any(low>high);
[unroll]for(uint p=0;p<6;p++){
float4 pl=planes[p];float3 v=float3(pl.x<0?low.x:high.x,pl.y<0?low.y:high.y,pl.z<0?low.z:high.z);
precise float x=pl.x*v.x;precise float y=pl.y*v.y;precise float z=pl.z*v.z;
precise float a=x+y;precise float b=z+pl.w;precise float d=a+b;if(d<0)visible=false;}
uint o=i*12;result[o]=asuint(low.x);result[o+1]=asuint(low.y);result[o+2]=asuint(low.z);result[o+3]=layer;
result[o+4]=asuint(high.x);result[o+5]=asuint(high.y);result[o+6]=asuint(high.z);result[o+7]=0;
result[o+8]=asuint(center.x);result[o+9]=asuint(center.y);result[o+10]=asuint(center.z);result[o+11]=visible?1:0;
if(visible){uint slot;InterlockedAdd(localCount,1,slot);ids[slot]=i;}}
GroupMemoryBarrierWithGroupSync();if(li==0)InterlockedAdd(result[count*13],localCount,base);
GroupMemoryBarrierWithGroupSync();if(li<localCount)result[count*12+base+li]=ids[li];
})";
            ComPtr<ID3DBlob> code,error,blob;
            auto hr=D3DCompile(shader,std::strlen(shader),"async-object-chain",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
            if(FAILED(hr)&&error)std::fprintf(stderr,"%s",(char*)error->GetBufferPointer());check(hr);
            D3D12_ROOT_PARAMETER p[4]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=25;
            p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[3].Descriptor.ShaderRegister=1;
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=4;rd.pParameters=p;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
            root.Reset();pso.Reset();check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
        }
        if(n>limit){
            if(pending)fail("capacity while pending");limit=n;cached.clear();
            upload=buffer(uint64_t(n)*64+4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            input=buffer(uint64_t(n)*64,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            metadataUpload=buffer(uint64_t(n)*32,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            metadata=buffer(uint64_t(n)*32,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            output=buffer(uint64_t(n*13+1)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            readback=buffer(uint64_t(n*13+1)*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    void begin(const Matrix* world,const AABB* boxes,uint32_t n,uint32_t start,const wi::primitive::Frustum& frustum){
        if(pending)fail("slot begin while pending");prepare(n);count=n;offset=start;
        auto begin=Clock::now();D3D12_RANGE empty{0,0};void* mapped{};
        uint32_t first=0,last=n;
        if(cached.size()==n){while(first<n&&!std::memcmp(&cached[first],&boxes[first],sizeof(AABB)))++first;while(last>first&&!std::memcmp(&cached[last-1],&boxes[last-1],sizeof(AABB)))--last;}
        check(upload->Map(0,&empty,&mapped));std::memcpy(mapped,world,size_t(n)*64);std::memset((char*)mapped+size_t(n)*64,0,4);upload->Unmap(0,nullptr);
        if(first<last){check(metadataUpload->Map(0,&empty,&mapped));std::memcpy((char*)mapped+size_t(first)*32,boxes+first,size_t(last-first)*32);metadataUpload->Unmap(0,nullptr);cached.assign(boxes,boxes+n);}
        check(allocator->Reset());check(list->Reset(allocator.Get(),pso.Get()));
        transition(input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(input.Get(),0,upload.Get(),0,uint64_t(n)*64);transition(input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(first<last){transition(metadata.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(metadata.Get(),uint64_t(first)*32,metadataUpload.Get(),uint64_t(first)*32,uint64_t(last-first)*32);transition(metadata.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(output.Get(),uint64_t(n)*52,upload.Get(),uint64_t(n)*64,4);transition(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());list->SetComputeRoot32BitConstants(0,24,frustum.planes,0);list->SetComputeRoot32BitConstant(0,n,24);
        list->SetComputeRootShaderResourceView(1,input->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(3,metadata->GetGPUVirtualAddress());
        list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);list->Dispatch((n+127)/128,1,1);list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,uint64_t(n)*52+4);transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamps.Get(),0);check(list->Close());ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);check(queue->Signal(fence.Get(),++serial));
        pending=true;submitted=Clock::now();ARCWickedCpuSample("Chain submit ms",ms(begin));ARCWickedCpuSample("Chain upload bytes",uint64_t(n)*64+uint64_t(last-first)*32+4);ARCWickedCpuSample("Chain metadata bytes",uint64_t(last-first)*32);
    }
    void finish(std::vector<Result>& results,std::vector<uint32_t>& visible){
        if(!pending)fail("slot finish without begin");double gap=ms(submitted);auto wait=Clock::now();
        if(fence->GetCompletedValue()<serial){check(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)ExitProcess(93);}
        ARCWickedCpuSample("Chain consumer wait ms",ms(wait));ARCWickedCpuSample("Chain independent gap ms",gap);
        auto commit=Clock::now();void* mapped{};D3D12_RANGE range{0,size_t(count)*52+4},empty{0,0};check(readback->Map(0,&range,&mapped));
        auto words=static_cast<const uint32_t*>(mapped);uint32_t length=words[count*13];if(length>count)ExitProcess(94);
        std::memcpy(results.data()+offset,mapped,size_t(count)*48);for(uint32_t i=0;i<length;++i){uint32_t local=words[count*12+i];if(local>=count)ExitProcess(94);visible.push_back(offset+local);}
        readback->Unmap(0,&empty);D3D12_RANGE qr{0,16};check(timestamps->Map(0,&qr,&mapped));auto stamps=static_cast<uint64_t*>(mapped);double kernel=double(stamps[1]-stamps[0])*1000/double(frequency);timestamps->Unmap(0,&empty);pending=false;
        ARCWickedCpuSample("Chain readback bytes",range.End);ARCWickedCpuSample("Chain commit ms",ms(commit));ARCWickedCpuSample("Chain kernel ms",kernel);
    }
};
struct Chain {
    std::array<Slot,2> slots;
    std::vector<Matrix> world;
    std::vector<AABB> boxes;
    std::vector<Result> results;
    std::vector<uint32_t> visible;
    wi::primitive::Frustum frustum{};
    wi::scene::Scene* owner{};
    int mode{},chunks{1},activeChunks{1};bool pending{},ready{};uint64_t generation{};
    void attach(ID3D12Device* device){char v[32]{};GetEnvironmentVariableA("ARC_ASYNC_CHAIN",v,32);mode=std::atoi(v);GetEnvironmentVariableA("ARC_CHAIN_CHUNKS",v,32);chunks=std::atoi(v)==2?2:1;if(mode)for(auto& s:slots)s.device=device;}
    bool begin(wi::scene::Scene* scene){
        if(!mode)return false;if(pending)fail("chain begin while pending");ready=false;owner=scene;++generation;
        uint32_t n=uint32_t(scene->objects.GetCount());if(!n||scene->softbodies.GetCount()||scene->emitters.GetCount()||scene->hairs.GetCount()||scene->impostors.GetCount())return false;activeChunks=std::min(chunks,int(n));
        auto prep=Clock::now();world.resize(n);boxes.resize(n);results.resize(n);visible.clear();visible.reserve(n);
        wi::jobsystem::context gather;std::atomic<bool> admitted{true};
        wi::jobsystem::Dispatch(gather,n,256,[&](wi::jobsystem::JobArgs args){
            uint32_t i=args.jobIndex;const auto& object=scene->objects[i];auto* mesh=scene->meshes.GetComponent(object.meshID);auto entity=scene->objects.GetEntity(i);auto* transform=scene->transforms.GetComponent(entity);
            if(!mesh||!transform||mesh->IsSkinned()||mesh->IsDynamic()){admitted.store(false,std::memory_order_relaxed);return;}
            world[i]=transform->world;boxes[i]=mesh->aabb;auto* layer=scene->layers.GetComponent(entity);boxes[i].layerMask=layer?layer->GetLayerMask():~0u;boxes[i].userdata=0;
        });
        wi::jobsystem::Wait(gather);if(!admitted.load(std::memory_order_relaxed))return false;
        frustum=scene->camera.frustum;ARCWickedCpuSample("Chain gather ms",ms(prep));
        uint32_t start=0;for(int c=0;c<activeChunks;++c){uint32_t length=(n-start)/uint32_t(activeChunks-c);if(!length)fail("empty partition");slots[c].begin(world.data()+start,boxes.data()+start,length,start,frustum);start+=length;}
        pending=true;ARCWickedCpuSample("Chain in flight packets",activeChunks);return true;
    }
    const Result* finish(wi::scene::Scene* scene){
        if(!pending||owner!=scene)return nullptr;for(int c=0;c<activeChunks;++c)slots[c].finish(results,visible);pending=false;
        if(mode==2){
            uint32_t bad=0;std::vector<uint8_t> seen(results.size());uint32_t expected=0;
            for(size_t i=0;i<results.size();++i){auto box=boxes[i].transform(world[i]);auto center=box.getCenter();auto& out=results[i];bool v=boxes[i].layerMask&&frustum.CheckBoxFast(box);
                if(std::memcmp(&box._min,&out.box._min,12)||std::memcmp(&box._max,&out.box._max,12)||std::memcmp(&center,&out.center,12)||out.visible!=uint32_t(v))++bad;expected+=v;}
            if(visible.size()!=expected)++bad;for(auto id:visible){if(id>=results.size()||!results[id].visible||seen[id])++bad;else seen[id]=1;}
            ARCWickedCpuSample("Chain oracle errors",bad);if(bad)ExitProcess(94);
        }
        ready=true;ARCWickedCpuSample("Chain CPU bounds skipped",results.size());return results.data();
    }
    int cull(const void* scene,uint32_t n,const wi::primitive::Frustum& f,uint32_t mask,uint32_t* out){
        if(!ready||owner!=scene||results.size()!=n||std::memcmp(&f,&frustum,sizeof(f)))return 0;
        for(uint32_t i=0;i<n;++i)out[i]=results[i].visible&&bool(results[i].box.layerMask&mask);
        if(mask==~0u){std::copy(visible.begin(),visible.end(),out+n);out[2*n]=uint32_t(visible.size());ARCWickedCpuSample("Chain CPU cull compact skipped",n);return 2;}
        return 1;
    }
};
inline Chain chain;
}
extern "C" bool ARCChainBegin(void* scene){return arc_chain::chain.begin(static_cast<wi::scene::Scene*>(scene));}
extern "C" const void* ARCChainFinish(void* scene){return arc_chain::chain.finish(static_cast<wi::scene::Scene*>(scene));}
extern "C" int ARCChainCull(const void* scene,uint32_t n,const void* frustum,uint32_t mask,uint32_t* out){return arc_chain::chain.cull(scene,n,*static_cast<const wi::primitive::Frustum*>(frustum),mask,out);}
