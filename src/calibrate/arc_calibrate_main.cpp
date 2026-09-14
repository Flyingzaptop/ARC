#include "arc/dx12_observer.hpp"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::cerr << "Cannot create DXGI factory\n";
        return 1;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter1;
        if (factory->EnumAdapters1(index, &adapter1) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter1->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) { continue; }
        ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter1.As(&adapter3))) { continue; }
        const auto budget = arc::dx12::query_memory_budget(adapter3.Get());
        std::wcout << L"{\n  \"schema\": 1,\n  \"gpu\": {\n    \"name\": \"" << description.Description
                   << L"\",\n    \"dedicated_vram_bytes\": " << description.DedicatedVideoMemory;
        if (budget) {
            std::wcout << L",\n    \"local_budget_bytes\": " << budget->local_budget
                       << L",\n    \"local_usage_bytes\": " << budget->local_usage;
        }
        std::wcout << L"\n  }\n}\n";
        return 0;
    }
    std::cerr << "No hardware DXGI adapter available\n";
    return 1;
}
