#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
#include "generic_shader_transform.hpp"

using Microsoft::WRL::ComPtr;
namespace {
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
}
int wmain(int argc, wchar_t** argv) try {
    if (argc != 5) throw std::runtime_error("Usage: arc-shader-tool dump|assemble|roundtrip|coarse2x2|coarse1x2|coarse2x1|neutral INPUT NEW_OUTPUT ABSOLUTE_DXCOMPILER_DLL");
    const std::wstring mode = argv[1];
    const bool controlled = mode == L"controlled";
    const bool transform = mode == L"coarse2x2" || mode == L"coarse1x2" || mode == L"coarse2x1" || mode == L"neutral" || controlled;
    if (mode != L"dump" && mode != L"assemble" && mode != L"roundtrip" && !transform) throw std::runtime_error("Unknown operation");
    const std::filesystem::path input = argv[2], output = argv[3], compiler_path = argv[4];
    if (!std::filesystem::is_regular_file(input) || std::filesystem::exists(output)) throw std::runtime_error("Input file and fresh output required");
    if (std::filesystem::file_size(input) > 32 * 1024 * 1024) throw std::runtime_error("Shader input exceeds 32 MiB");
    std::ifstream file(input, std::ios::binary); std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
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
    if (transform) {
        const auto transformed = arc::dx12::shader::coarse_compute(
            {static_cast<const char*>(source->GetBufferPointer()),source->GetBufferSize()},
            mode == L"coarse2x2" || mode == L"coarse2x1" ? 2 : 1,
            mode == L"coarse2x2" || mode == L"coarse1x2" ? 2 : 1, controlled);
        if (!transformed.admitted) throw std::runtime_error("Shader declined: " + transformed.reason);
        control_space=transformed.control_space;
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
    std::cout << "{\"input_bytes\":" << bytes.size() << ",\"output_bytes\":" << generated->GetBufferSize()
        << ",\"control_space\":" << control_space << ",\"validated\":" << (mode == L"dump" ? "false" : "true") << "}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
