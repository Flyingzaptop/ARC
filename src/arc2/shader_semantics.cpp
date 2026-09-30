#include "shader_semantics.hpp"
#include "arc/arc2/runtime.hpp"
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
namespace arc::arc2 {
namespace {
std::mutex shader_mutex;
std::map<std::string,ObjectId> shaders;
ObjectId shader(D3D12_SHADER_BYTECODE code,const char* stage){
 if(!code.pShaderBytecode||!code.BytecodeLength)return {};
 // No speculative reads past the original API's bytecode extent.
 if(code.BytecodeLength>64*1024*1024){runtime().unsupported({},"shader bytecode budget");return {};}
 BCRYPT_ALG_HANDLE alg{};std::array<unsigned char,32> digest{};
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0){runtime().unsupported({},"SHA256 unavailable");return {};}
 auto status=BCryptHash(alg,nullptr,0,(PUCHAR)code.pShaderBytecode,static_cast<ULONG>(code.BytecodeLength),digest.data(),static_cast<ULONG>(digest.size()));BCryptCloseAlgorithmProvider(alg,0);if(status<0)return {};
 std::string key=stage;key+=':';constexpr char digits[]="0123456789abcdef";for(auto b:digest){key+=digits[b>>4];key+=digits[b&15];}
 std::lock_guard lock(shader_mutex);if(auto it=shaders.find(key);it!=shaders.end())return it->second;
 if(shaders.size()>=4096){runtime().unsupported({},"shader identity budget");return {};}
 auto id=runtime().create_object(ObjectKind::Shader,0);std::vector<Access> access;std::vector<ShaderBinding> bindings;
 Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
 HRESULT hr=D3DReflect(code.pShaderBytecode,code.BytecodeLength,IID_PPV_ARGS(&reflection));
 D3D12_SHADER_DESC description{};
 if(SUCCEEDED(hr)&&SUCCEEDED(reflection->GetDesc(&description))){
  for(UINT i=0;i<description.BoundResources;++i){D3D12_SHADER_INPUT_BIND_DESC b{};if(FAILED(reflection->GetResourceBindingDesc(i,&b))){access.push_back({{},AccessKind::Unknown,Certainty::Unknown,0,0,key+":reflection failure"});continue;}
   auto kind=AccessKind::Read;switch(b.Type){case D3D_SIT_UAV_RWTYPED:case D3D_SIT_UAV_RWSTRUCTURED:case D3D_SIT_UAV_RWBYTEADDRESS:case D3D_SIT_UAV_APPEND_STRUCTURED:case D3D_SIT_UAV_CONSUME_STRUCTURED:case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:kind=AccessKind::ReadWrite;break;default:break;}
   if(b.Type==D3D_SIT_SAMPLER)continue;
   BindingKind binding=kind==AccessKind::ReadWrite?BindingKind::Uav:(b.Type==D3D_SIT_CBUFFER?BindingKind::Cbv:BindingKind::Srv);
   bindings.push_back({binding,b.Space,b.BindPoint,b.BindCount,true,kind});
   auto symbol=key+":space="+std::to_string(b.Space)+",register="+std::to_string(b.BindPoint)+",count="+std::to_string(b.BindCount)+",type="+std::to_string(b.Type)+",indexing=may-access-declared-range";
   access.push_back({{},kind,Certainty::Symbolic,b.BindPoint,b.BindCount,symbol});
  }
 }else access.push_back({{},AccessKind::Unknown,Certainty::Unknown,0,0,key+":reflection unavailable; DXIL/container access remains unknown"});
 // Identity persists even for a shader with no declared resources.
 if(access.empty())access.push_back({{},AccessKind::Unknown,Certainty::Symbolic,0,0,key+":no declared bindings; instruction effects not proven"});
 runtime().describe_shader(id,access);runtime().describe_shader_bindings(id,bindings);shaders.emplace(key,id);return id;
}
}
ObjectId register_shader_stage(D3D12_SHADER_BYTECODE code,const char* stage){return shader(code,stage);}
void describe_graphics_pipeline(ObjectId id,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d){describe_fixed_pipeline(id,d);std::vector<ObjectId> ids;for(auto p:std::array<std::pair<D3D12_SHADER_BYTECODE,const char*>,5>{{{d.VS,"vs"},{d.PS,"ps"},{d.DS,"ds"},{d.HS,"hs"},{d.GS,"gs"}}})if(auto s=shader(p.first,p.second))ids.push_back(s);runtime().set_pipeline_shaders(id,ids);}
void describe_compute_pipeline(ObjectId id,const D3D12_COMPUTE_PIPELINE_STATE_DESC& d){auto s=shader(d.CS,"cs");if(s)runtime().set_pipeline_shaders(id,std::span<const ObjectId>(&s,1));}
}
