#include "generic_spatial_probe.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#include <string>
namespace arc::dx12::optimizer {
namespace {
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("Spatial probe HRESULT "+std::to_string(h));}
const char* shader=R"(
cbuffer Control:register(b0){uint4 c[8];}
cbuffer Meta:register(b1){uint outputs;uint require_features;}
RWByteAddressBuffer original:register(u0),candidate:register(u1),result:register(u2),features:register(u3);
groupshared float difference[64],reference_scale[64];groupshared uint channels[64],invalid[64],stale[64];
[numthreads(64,1,1)]void Reset(uint3 id:SV_DispatchThreadID){if(!(c[7].x|c[7].y))return;if(id.x<256)result.Store4(16+id.x*16,0);if(!id.x){result.Store4(0,uint4(c[7].xy,c[5].y,outputs));result.Store4(4112,0);}}
[numthreads(64,1,1)]void Compare(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    if(!(c[7].x|c[7].y)||!outputs||outputs>8||c[7].z<16||c[7].z>8192||c[7].w>=c[7].z)return;
    uint2 extent=c[0].zw,tile=c[6].xy;if(!all(extent)||!all(tile)||any(tile>16384)||tile.x*tile.y>65536)return;
    uint2 tiles=(extent+tile-1)/tile;uint index=group.x*c[7].z+c[7].w;if(index>=tiles.x*tiles.y||index>=8192)return;
    uint2 origin=uint2(index%tiles.x,index/tiles.x)*tile;uint area=tile.x*tile.y;
    float error=0;uint bad=0,old=0;
    for(uint output=0;output<outputs;++output){float sum=0,scale=0;uint count=0,invalid_pixel=0,stale_pixel=0;
        for(uint i=lane;i<area;i+=64){uint2 p=origin+uint2(i%tile.x,i/tile.x);if(any(p>=extent))continue;
            uint address=((group.x*area+i)*outputs+output)*32;uint4 a=original.Load4(address+16),b=candidate.Load4(address+16);
            if(any(a.xy!=c[7].xy)||any(b.xy!=c[7].xy)){stale_pixel=1;continue;}if(a.z!=b.z){invalid_pixel=1;continue;}
            float4 va=asfloat(original.Load4(address)),vb=asfloat(candidate.Load4(address));
            [unroll]for(uint channel=0;channel<4;++channel)if(a.z&(1u<<channel)){if(!isfinite(va[channel])||!isfinite(vb[channel]))invalid_pixel=1;else{sum+=abs(va[channel]-vb[channel]);scale+=abs(va[channel]);++count;}}
        }
        difference[lane]=sum;reference_scale[lane]=scale;channels[lane]=count;invalid[lane]=invalid_pixel;stale[lane]=stale_pixel;GroupMemoryBarrierWithGroupSync();
        for(uint stride=32;stride;stride>>=1){if(lane<stride){difference[lane]+=difference[lane+stride];reference_scale[lane]+=reference_scale[lane+stride];channels[lane]+=channels[lane+stride];invalid[lane]|=invalid[lane+stride];stale[lane]|=stale[lane+stride];}GroupMemoryBarrierWithGroupSync();}
        if(!lane){float n=max(1,channels[0]);if(!isfinite(difference[0])||!isfinite(reference_scale[0]))bad=1;else error=max(error,(difference[0]/n)/max(1,reference_scale[0]/n));bad|=invalid[0];old|=stale[0];}GroupMemoryBarrierWithGroupSync();
    }
    if(lane)return;uint ignored;if(old){result.InterlockedAdd(4116,1,ignored);return;}
    uint bin=0;
    if(require_features){uint address=32+index*32+(c[6].z&1)*262144;uint4 header=features.Load4(address),data=features.Load4(address+16);
        bool learned=(c[6].z&32)!=0,known=learned?(header.w&0x80000000)!=0:data.z==asuint(1.);
        if(c[5].x!=8192||any(header.xy!=c[5].zw)||header.z!=c[5].y||!known){result.InterlockedAdd(4112,1,ignored);return;}
        if(learned)bin=(header.w>>8)&255;
        else{float importance=asfloat(data.x),mean=abs(asfloat(data.y)),motion=asfloat(data.w);if(!isfinite(importance)||!isfinite(mean)||!isfinite(motion)){result.InterlockedAdd(4112,1,ignored);return;}
            uint a=min(7,(uint)(saturate(importance/2)*8)),b=min(7,(uint)(saturate(mean/(1+mean))*8)),d=motion<.001?0:motion<.01?1:motion<.1?2:3;bin=a+8*b+64*d;}
    }
    uint address=16+bin*16;if(bad){result.InterlockedAdd(address+8,1,ignored);return;}
    result.InterlockedMax(address,asuint(error),ignored);result.InterlockedAdd(address+4,1,ignored);
}
)";
}
SpatialProbeGpu::SpatialProbeGpu(ID3D12Device* device,UINT64 bytes):capacity_(bytes){
    if(!device||!bytes||bytes>64ull*1024*1024)throw std::runtime_error("Spatial probe allocation capacity");
    auto resource=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;Ptr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
    reference_=resource(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);candidate_=resource(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);result_=resource(sizeof(Observation),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);readback_=resource(sizeof(Observation),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    for(auto* resource:{reference_.Get(),candidate_.Get(),result_.Get(),readback_.Get()}){const auto desc=resource->GetDesc();const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(bytes!=UINT64_MAX)allocation_bytes_[resource==readback_.Get()?2:0]+=bytes;}
    D3D12_ROOT_PARAMETER parameters[6]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor={0,0};for(unsigned i=0;i<4;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[i+1].Descriptor={i,0};}parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[5].Constants={1,0,2};D3D12_ROOT_SIGNATURE_DESC desc{6,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};Ptr<ID3DBlob> root;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&root,nullptr));check(device->CreateRootSignature(0,root->GetBufferPointer(),root->GetBufferSize(),IID_PPV_ARGS(&root_)));
    for(unsigned i=0;i<2;++i){Ptr<ID3DBlob> code,errors;const auto hr=D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,i?"Compare":"Reset","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);if(FAILED(hr))throw std::runtime_error(errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Spatial compare compilation");D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root_.Get();p.CS={code->GetBufferPointer(),code->GetBufferSize()};Ptr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipeline)));if(i)compare_=std::move(pipeline);else reset_=std::move(pipeline);}
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)));check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&copy_)));
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={result_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};copy_->ResourceBarrier(1,&barrier);copy_->CopyResource(readback_.Get(),result_.Get());std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);copy_->ResourceBarrier(1,&barrier);check(copy_->Close());check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)));
}
bool SpatialProbeGpu::reserve(UINT64 epoch){if(!epoch||epoch<=expected_epoch_||!available())return false;expected_epoch_=epoch;pending_=true;submitted_=false;return true;}
void SpatialProbeGpu::record_compare(ID3D12GraphicsCommandList* list,D3D12_GPU_VIRTUAL_ADDRESS control,D3D12_GPU_VIRTUAL_ADDRESS map,unsigned outputs,bool require_features){
    D3D12_RESOURCE_BARRIER barriers[2]{};for(unsigned i=0;i<2;++i){barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barriers[i].UAV.pResource=i?candidate_.Get():reference_.Get();}list->ResourceBarrier(2,barriers);
    list->SetComputeRootSignature(root_.Get());list->SetComputeRootConstantBufferView(0,control);list->SetComputeRootUnorderedAccessView(1,reference_address());list->SetComputeRootUnorderedAccessView(2,candidate_address());list->SetComputeRootUnorderedAccessView(3,result_->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(4,map);const UINT meta[]{outputs,require_features?1u:0u};list->SetComputeRoot32BitConstants(5,2,meta,0);list->SetPipelineState(reset_.Get());list->Dispatch(4,1,1);barriers[0].UAV.pResource=result_.Get();list->ResourceBarrier(1,barriers);list->SetPipelineState(compare_.Get());list->Dispatch(512,1,1);list->ResourceBarrier(1,barriers);
}
void SpatialProbeGpu::submitted(ID3D12CommandQueue* queue){if(!pending_||fault_||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::runtime_error("Spatial probe submission");submitted_=true;ID3D12CommandList* copy[]{copy_.Get()};queue->ExecuteCommandLists(1,copy);if(FAILED(queue->Signal(fence_.Get(),++sequence_))){fault_=true;throw std::runtime_error("Spatial probe fence");}}
std::optional<SpatialProbeGpu::Observation> SpatialProbeGpu::collect(){if(!pending_||fault_||!submitted_||!sequence_)return {};const auto completed=fence_->GetCompletedValue();if(completed==UINT64_MAX){fault_=true;return {};}if(completed<sequence_)return {};Observation result{};D3D12_RANGE range{0,sizeof(result)};void* data{};if(FAILED(readback_->Map(0,&range,&data))){fault_=true;return {};}std::memcpy(&result,data,sizeof(result));D3D12_RANGE none{};readback_->Unmap(0,&none);pending_=false;if(result.epoch!=expected_epoch_)return {};return result;}
}
