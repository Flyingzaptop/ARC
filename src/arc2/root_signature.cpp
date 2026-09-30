#include "shader_semantics.hpp"
#include "arc/arc2/runtime.hpp"
#include <wrl/client.h>
namespace arc::arc2 {
namespace {
BindingKind kind(D3D12_DESCRIPTOR_RANGE_TYPE v){switch(v){case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:return BindingKind::Cbv;case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:return BindingKind::Uav;case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:return BindingKind::Sampler;default:return BindingKind::Srv;}}
template<class Description> std::vector<RootParameter> parameters(const Description& description){
 std::vector<RootParameter> result;
 if(description.NumParameters>64)return result;
 for(UINT i=0;i<description.NumParameters;++i){const auto& p=description.pParameters[i];RootParameter r;r.slot=i;
  if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){r.kind=RootParameterKind::Table;for(UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j){const auto& range=p.DescriptorTable.pDescriptorRanges[j];r.ranges.push_back({kind(range.RangeType),range.RegisterSpace,range.BaseShaderRegister,range.NumDescriptors,range.OffsetInDescriptorsFromTableStart});}}
  else if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){r.kind=RootParameterKind::Constants;r.space=p.Constants.RegisterSpace;r.shader_register=p.Constants.ShaderRegister;r.constant_count=p.Constants.Num32BitValues;}
  else {r.kind=RootParameterKind::Descriptor;r.space=p.Descriptor.RegisterSpace;r.shader_register=p.Descriptor.ShaderRegister;r.descriptor_kind=p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV?BindingKind::Cbv:p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_UAV?BindingKind::Uav:BindingKind::Srv;}
  result.push_back(std::move(r));
 }return result;
}
}
void describe_root_signature(ObjectId id,const void* bytes,SIZE_T size){
 Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> decoded;
 if(!bytes||!size||FAILED(D3D12CreateVersionedRootSignatureDeserializer(bytes,size,IID_PPV_ARGS(&decoded)))){runtime().unsupported({},"root signature deserialization unavailable");return;}
 auto d=decoded->GetUnconvertedRootSignatureDesc();std::vector<RootParameter> result;
 if(d->Version==D3D_ROOT_SIGNATURE_VERSION_1_0)result=parameters(d->Desc_1_0);
 else if(d->Version==D3D_ROOT_SIGNATURE_VERSION_1_1)result=parameters(d->Desc_1_1);
 else{runtime().unsupported({},"root signature version unsupported");return;}
 runtime().describe_root_signature(id,result);
}
}
