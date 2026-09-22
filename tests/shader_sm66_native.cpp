#include "generic_shader_transform.hpp"
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void hr(HRESULT result){require(SUCCEEDED(result),"DXC HRESULT");}
ComPtr<IDxcBlob> operation_result(IDxcOperationResult* operation){
    HRESULT status{};hr(operation->GetStatus(&status));
    if(FAILED(status)){ComPtr<IDxcBlobEncoding> errors;operation->GetErrorBuffer(&errors);
        if(errors)std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());hr(status);}
    ComPtr<IDxcBlob> result;hr(operation->GetResult(&result));return result;
}
std::string replace_one(std::string source,const std::string& from,const std::string& to){
    const auto at=source.find(from);require(at!=std::string::npos,"Fixture IR shape");source.replace(at,from.size(),to);return source;
}
struct Dxc {
    HMODULE compiler_module{},validator_module{};
    ComPtr<IDxcLibrary> library;ComPtr<IDxcCompiler> compiler;ComPtr<IDxcAssembler> assembler;ComPtr<IDxcValidator> validator;
    explicit Dxc(const std::filesystem::path& path){
        require(path.is_absolute(),"Absolute DXC DLL path required");
        constexpr auto flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
        validator_module=LoadLibraryExW((path.parent_path()/"dxil.dll").c_str(),nullptr,flags);
        compiler_module=LoadLibraryExW(path.c_str(),nullptr,flags);require(validator_module&&compiler_module,"Load DXC and validator");
        auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler_module,"DxcCreateInstance"));
        auto validate_create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator_module,"DxcCreateInstance"));
        require(create&&validate_create,"DXC factory");
        hr(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)));hr(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));
        hr(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler)));hr(validate_create(CLSID_DxcValidator,IID_PPV_ARGS(&validator)));
    }
    ~Dxc(){validator.Reset();assembler.Reset();compiler.Reset();library.Reset();if(compiler_module)FreeLibrary(compiler_module);if(validator_module)FreeLibrary(validator_module);}
    ComPtr<IDxcBlobEncoding> text(const std::string& source){ComPtr<IDxcBlobEncoding> blob;
        hr(library->CreateBlobWithEncodingOnHeapCopy(source.data(),static_cast<UINT32>(source.size()),CP_UTF8,&blob));return blob;}
    std::string compile(const std::string& source){auto blob=text(source);ComPtr<IDxcOperationResult> operation;
        hr(compiler->Compile(blob.Get(),L"sm66",L"MainCS",L"cs_6_6",nullptr,0,nullptr,0,nullptr,&operation));
        auto binary=operation_result(operation.Get());ComPtr<IDxcBlobEncoding> disassembly;hr(compiler->Disassemble(binary.Get(),&disassembly));
        return {static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize()};}
    void validate(const std::string& ir){auto blob=text(ir);ComPtr<IDxcOperationResult> operation;
        hr(assembler->AssembleToContainer(blob.Get(),&operation));auto binary=operation_result(operation.Get());
        operation.Reset();hr(validator->Validate(binary.Get(),DxcValidatorFlags_InPlaceEdit,&operation));operation_result(operation.Get());}
};
}

int wmain(int argc,wchar_t** argv)try{
    require(argc==2,"Usage: shader-sm66-native ABSOLUTE_DXCOMPILER_DLL");Dxc dxc(argv[1]);
    const auto ir=dxc.compile(R"(RWTexture2D<float4> output:register(u0);
        [numthreads(8,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=float4(p.xy,0,1);})");
    require(ir.find("@dx.op.createHandleFromBinding")!=ir.npos&&ir.find("@dx.op.annotateHandle")!=ir.npos,"SM6.6 binding fixture");
    for(const auto [x,y,controlled,proof]:{std::tuple<unsigned,unsigned,bool,bool>{1,1,false,false},{2,2,false,false},
        {1,1,true,false},{2,2,true,true}}){
        auto transformed=arc::dx12::shader::coarse_compute(ir,x,y,controlled,UINT32_MAX,proof);
        require(transformed.admitted,transformed.reason.c_str());
        require(transformed.ir.find("@dx.op.createHandle(i32 57")==transformed.ir.npos,"SM6.6 generated legacy handle");
        dxc.validate(transformed.ir);
    }
    const auto inputs=dxc.compile(R"(cbuffer C:register(b3,space2){float4 k;}
        Texture2D<float4> input:register(t2,space1);RWTexture2D<float4> output:register(u4,space3);
        [numthreads(8,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=input[p.xy]+k;})");
    auto controlled=arc::dx12::shader::coarse_compute(inputs,1,1,true);
    require(controlled.admitted,controlled.reason.c_str());require(controlled.resources.size()==3,"Static resource contracts");dxc.validate(controlled.ir);
    auto coarse=arc::dx12::shader::coarse_compute(inputs,2,2);
    require(coarse.admitted,coarse.reason.c_str());dxc.validate(coarse.ir);
    const auto sampled=dxc.compile(R"(cbuffer C:register(b3,space2){float4 k;}
        Texture2D<float4> input:register(t2,space1);SamplerState samp:register(s0);
        RWTexture2D<float4> output:register(u4,space3);
        [numthreads(8,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=input.SampleLevel(samp,float2(p.xy)/32,0)+k;})");
    auto sampled_proof=arc::dx12::shader::coarse_compute(sampled,1,1,true,4,true);
    require(sampled_proof.admitted,sampled_proof.reason.c_str());dxc.validate(sampled_proof.ir);
    const auto zero_binding=dxc.compile(R"(Texture2D<float4> input:register(t0);SamplerState samp:register(s0);
        RWTexture2D<float4> output:register(u0);
        [numthreads(8,8,1)]void MainCS(uint3 p:SV_DispatchThreadID){output[p.xy]=input.SampleLevel(samp,float2(p.xy)/32,0);})");
    require(zero_binding.find("%dx.types.ResBind zeroinitializer")!=zero_binding.npos,"DXC zero binding fixture");
    auto zero_binding_coarse=arc::dx12::shader::coarse_compute(zero_binding,2,2);
    require(zero_binding_coarse.admitted,zero_binding_coarse.reason.c_str());dxc.validate(zero_binding_coarse.ir);
    auto declined=[&](const std::string& candidate){require(!arc::dx12::shader::coarse_compute(candidate,2,2,true).admitted,"Unknown binding must decline");};
    declined(replace_one(ir,"%dx.types.ResBind { i32 0, i32 0, i32 0, i8 1 }","%dx.types.ResBind { i32 0, i32 1, i32 0, i8 1 }"));
    declined(replace_one(ir,"%dx.types.ResBind { i32 0, i32 0, i32 0, i8 1 }","%dx.types.ResBind { i32 0, i32 0, i32 7, i8 1 }"));
    declined(replace_one(ir,"%dx.types.ResBind { i32 0, i32 0, i32 0, i8 1 }","%dx.types.ResBind { i32 0, i32 0, i32 0, i8 0 }"));
    declined(replace_one(ir,"%dx.types.ResourceProperties { i32 4098, i32 1033 }","%dx.types.ResourceProperties { i32 2, i32 1033 }"));
    declined(replace_one(ir,"@dx.op.createHandleFromBinding(i32 217,","@dx.op.createHandleFromHeap(i32 218,"));
    const auto unsupported=dxc.compile(R"(RWByteAddressBuffer output:register(u0);
        [numthreads(8,8,1)] void MainCS(uint3 p:SV_DispatchThreadID){output.Store(0,p.x);})");
    declined(unsupported); // Side-effect shape is outside the texture-store proof.
    std::cout<<"SM6.6 static bindings, DXIL validation, and declines passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
