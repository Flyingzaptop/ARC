#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include "generic_shader_transform.hpp"
#include "generic_pcf_transform.hpp"
#include "generic_zero_transform.hpp"
#include "generic_ir_hints.hpp"
#include "generic_edge_transform.hpp"
#include "generic_mip_transform.hpp"

using Microsoft::WRL::ComPtr;
namespace {
struct __declspec(uuid("5F956ED5-78D1-4B15-8247-F7187614A041")) DxbcConverter: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Convert(LPCVOID,UINT32,LPCWSTR,LPVOID*,UINT32*,LPWSTR*)=0;
};
constexpr CLSID converter_class{0x4900391e,0xb752,0x4edd,{0xa8,0x85,0x6f,0xb7,0x6e,0x25,0xad,0xdb}};
struct Module {
    HMODULE handle{};
    explicit Module(const std::filesystem::path& path) {
        if (!path.is_absolute()) throw std::runtime_error("Compiler path must be absolute");
        handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle) throw std::runtime_error("Cannot load compiler component");
    }
    ~Module() { if (handle) FreeLibrary(handle); }
    Module(const Module&) = delete;
};
void require(HRESULT value, const char* message) { if (FAILED(value)) throw std::runtime_error(message); }
ComPtr<IDxcBlob> result(IDxcOperationResult* operation) {
    HRESULT status{}; require(operation->GetStatus(&status), "Operation status unavailable");
    if (FAILED(status)) {
        ComPtr<IDxcBlobEncoding> errors; operation->GetErrorBuffer(&errors);
        if (errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        throw std::runtime_error("Shader operation failed");
    }
    ComPtr<IDxcBlob> blob; require(operation->GetResult(&blob), "Missing shader result"); return blob;
}
bool has_dxil(const std::vector<char>& bytes){
    if(bytes.size()<32||std::memcmp(bytes.data(),"DXBC",4)!=0)throw std::runtime_error("Shader container expected");
    UINT32 count{};std::memcpy(&count,bytes.data()+28,4);if(count>64||32+std::size_t(count)*4>bytes.size())throw std::runtime_error("Shader chunk table");
    for(UINT32 i=0;i<count;++i){UINT32 offset{};std::memcpy(&offset,bytes.data()+32+i*4,4);if(offset>bytes.size()-8)throw std::runtime_error("Shader chunk bounds");if(std::memcmp(bytes.data()+offset,"DXIL",4)==0)return true;}
    return false;
}
void convert_dxbc(std::vector<char>& bytes){
    wchar_t system[MAX_PATH]{};const auto length=GetSystemDirectoryW(system,MAX_PATH);if(!length||length>=MAX_PATH)throw std::runtime_error("System directory unavailable");
    Module converter_module(std::filesystem::path(system)/"dxilconv.dll");auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(converter_module.handle,"DxcCreateInstance"));if(!create)throw std::runtime_error("DXBC converter entry point");
    ComPtr<DxbcConverter> converter;require(create(converter_class,IID_PPV_ARGS(&converter)),"Create DXBC converter");
    void* data{};UINT32 size{};wchar_t* diagnostics{};const auto status=converter->Convert(bytes.data(),static_cast<UINT32>(bytes.size()),nullptr,&data,&size,&diagnostics);
    if(diagnostics){std::wcerr<<diagnostics;CoTaskMemFree(diagnostics);}
    if(FAILED(status)||!data||!size||size>32*1024*1024){CoTaskMemFree(data);throw std::runtime_error("DXBC conversion failed");}
    try{const auto* start=static_cast<const char*>(data);bytes.assign(start,start+size);}catch(...){CoTaskMemFree(data);throw;}CoTaskMemFree(data);
    if(!has_dxil(bytes))throw std::runtime_error("Converter did not produce DXIL");
}
}
int wmain(int argc, wchar_t** argv) try {
    if (argc != 5) throw std::runtime_error("Usage: arc-shader-tool dump|assemble|roundtrip|coarse2x2|coarse1x2|coarse2x1|neutral INPUT NEW_OUTPUT ABSOLUTE_DXCOMPILER_DLL");
    const std::wstring mode = argv[1];
    const bool proof=mode==L"controlled-proof"||mode.starts_with(L"controlled-proof:");
    const bool controlled = proof || mode == L"controlled" || mode.starts_with(L"controlled:");
    unsigned requested_space=UINT32_MAX;
    if(mode.find(L':')!=mode.npos&&controlled){std::size_t used{};const auto suffix=mode.substr(mode.find(L':')+1);const auto value=std::stoul(suffix,&used);if(used!=suffix.size()||value>=65536)throw std::runtime_error("Control space must be 0..65535");requested_space=static_cast<unsigned>(value);}
    const bool transform = mode == L"coarse2x2" || mode == L"coarse1x2" || mode == L"coarse2x1" || mode == L"neutral" || controlled;
    if (mode != L"dump" && mode != L"assemble" && mode != L"roundtrip" && !transform) throw std::runtime_error("Unknown operation");
    const std::filesystem::path input = argv[2], output = argv[3], compiler_path = argv[4];
    if (!std::filesystem::is_regular_file(input) || std::filesystem::exists(output)) throw std::runtime_error("Input file and fresh output required");
    if (std::filesystem::file_size(input) > 32 * 1024 * 1024) throw std::runtime_error("Shader input exceeds 32 MiB");
    std::ifstream file(input, std::ios::binary); std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    const auto original_bytes=bytes.size();const bool converted=mode!=L"assemble"&&!has_dxil(bytes);if(converted)convert_dxbc(bytes);
    Module validator_module(compiler_path.parent_path() / "dxil.dll"), compiler(compiler_path);
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler.handle, "DxcCreateInstance"));
    auto create_validator = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator_module.handle, "DxcCreateInstance"));
    if (!create || !create_validator) throw std::runtime_error("Compiler entry points unavailable");
    ComPtr<IDxcLibrary> library; require(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library)), "Create DXC library");
    ComPtr<IDxcBlobEncoding> source; require(library->CreateBlobWithEncodingOnHeapCopy(bytes.data(), static_cast<UINT32>(bytes.size()),
        mode == L"assemble" ? CP_UTF8 : 0, &source), "Create shader blob");
    ComPtr<IDxcBlob> generated;
    if (mode != L"assemble") {
        ComPtr<IDxcCompiler> disassembler; require(create(CLSID_DxcCompiler, IID_PPV_ARGS(&disassembler)), "Create disassembler");
        ComPtr<IDxcBlobEncoding> text; require(disassembler->Disassemble(source.Get(), &text), "Disassemble shader");
        if (mode == L"dump") generated = text;
        else source = text;
    }
    unsigned control_space=UINT32_MAX;
    if(converted&&mode!=L"dump"){
        const auto normalized=arc::dx12::shader::normalize_converted_dxil({static_cast<const char*>(source->GetBufferPointer()),source->GetBufferSize()});
        source.Reset();require(library->CreateBlobWithEncodingOnHeapCopy(normalized.data(),static_cast<UINT32>(normalized.size()),CP_UTF8,&source),"Create normalized converter IR");
    }
    arc::dx12::shader::Transform contract;
    std::string original_ir;
    if (transform) {
        if(controlled)original_ir.assign(static_cast<const char*>(source->GetBufferPointer()),source->GetBufferSize());
        auto transformed = arc::dx12::shader::coarse_compute(
            {static_cast<const char*>(source->GetBufferPointer()),source->GetBufferSize()},
            mode == L"coarse2x2" || mode == L"coarse2x1" ? 2 : 1,
            mode == L"coarse2x2" || mode == L"coarse1x2" ? 2 : 1, controlled, requested_space, proof);
        if (!transformed.admitted) throw std::runtime_error("Shader declined: " + transformed.reason);
        if(controlled){auto zero=arc::dx12::shader::short_circuit_zero_factors(transformed.ir);transformed.ir=std::move(zero.ir);transformed.zero_factor_regions=zero.regions;auto filtered=arc::dx12::shader::sparse_comparison_filter(transformed.ir);transformed.ir=std::move(filtered.ir);transformed.comparison_filter_groups=filtered.groups;}
        if(controlled){auto edges=arc::dx12::shader::protect_input_edges(transformed.ir,transformed);transformed.ir=std::move(edges.ir);transformed.edge_input_mask=edges.input_mask;}
        if(controlled){auto mips=arc::dx12::shader::bias_explicit_mips(transformed.ir);transformed.ir=std::move(mips.ir);transformed.mip_samples=mips.samples;}
        transformed.ir=arc::dx12::shader::preserve_arc_branches(std::move(transformed.ir));
        control_space=transformed.control_space;
        contract=transformed;contract.ir.clear();
        source.Reset();require(library->CreateBlobWithEncodingOnHeapCopy(transformed.ir.data(), static_cast<UINT32>(transformed.ir.size()), CP_UTF8, &source), "Create transformed blob");
    }
    if (mode != L"dump") {
        ComPtr<IDxcAssembler> assembler; require(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler)), "Create assembler");
        ComPtr<IDxcOperationResult> assembled; require(assembler->AssembleToContainer(source.Get(), &assembled), "Assemble DXIL");
        generated = result(assembled.Get());
        ComPtr<IDxcValidator> validator; require(create_validator(CLSID_DxcValidator, IID_PPV_ARGS(&validator)), "Create validator");
        ComPtr<IDxcOperationResult> validated; require(validator->Validate(generated.Get(), DxcValidatorFlags_InPlaceEdit, &validated), "Validate DXIL");
        generated = result(validated.Get());
    }
    std::ofstream target(output, std::ios::binary); target.write(static_cast<const char*>(generated->GetBufferPointer()), generated->GetBufferSize()); target.close();
    if (!target) throw std::runtime_error("Write shader output");
    if(transform){
        auto manifest_path=output;manifest_path+=L".contract";
        if(std::filesystem::exists(manifest_path))throw std::runtime_error("Fresh contract output required");
        std::ofstream manifest(manifest_path);
        manifest<<(proof?"ARC_SHADER_CONTRACT_7\n":"ARC_SHADER_CONTRACT_5\n")<<contract.control_space<<' '<<contract.threads[0]<<' '<<contract.threads[1]<<' '<<contract.threads[2]<<' '<<contract.stores<<' '<<contract.resources.size()<<' '<<contract.comparison_filter_groups<<' '<<contract.zero_factor_regions<<' '<<contract.edge_input_mask<<' '<<contract.mip_samples;
        if(proof)manifest<<' '<<contract.execution_marker;manifest<<'\n';
        for(const auto& r:contract.resources)manifest<<r.resource_class<<' '<<r.range_id<<' '<<r.shader_register<<' '<<r.space<<' '<<r.count<<' '<<r.kind<<'\n';
        manifest.close();if(!manifest)throw std::runtime_error("Write shader contract");
        if(controlled){auto access_path=output;access_path+=L".access.ll";if(std::filesystem::exists(access_path))throw std::runtime_error("Fresh access program required");std::ofstream access(access_path,std::ios::binary);access.write(original_ir.data(),original_ir.size());access.close();if(!access)throw std::runtime_error("Write access program");}
    }
    std::cout << "{\"input_bytes\":" << original_bytes << ",\"output_bytes\":" << generated->GetBufferSize()
        << ",\"control_space\":" << control_space << ",\"converted_from_dxbc\":"<<(converted?"true":"false")<<",\"validated\":" << (mode == L"dump" ? "false" : "true") << "}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
