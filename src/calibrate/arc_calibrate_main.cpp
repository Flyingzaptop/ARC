#include "arc/dx12_observer.hpp"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <filesystem>
#include <iostream>

#include "benchmarks.hpp"

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) try {
    std::filesystem::path storage_input = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(argv[0]);
    if (!std::filesystem::exists(storage_input) || !std::filesystem::is_regular_file(storage_input)) {
        // Calibration callers may provide a disposable scratch path. Falling back
        // to the executable keeps calibration useful instead of failing before
        // hardware discovery. The profile records the actual bytes read.
        storage_input = std::filesystem::path(argv[0]);
    }
    const auto benchmarkJson = calibration::run(storage_input);

    std::wofstream output(argc > 1 ? argv[1] : "hardware-profile.json");
    if (!output) {
        throw std::runtime_error("cannot write hardware profile");
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    DEVMODEW display{};
    display.dmSize = sizeof(display);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::cerr << "Cannot create DXGI factory\n";
        return 1;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter1;
        if (factory->EnumAdapters1(index, &adapter1) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter1->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter1.As(&adapter3))) {
            continue;
        }

        const auto budget = arc::dx12::query_memory_budget(adapter3.Get());
        output << L"{\n  \"schema\": 2,\n  \"gpu\": {\n    \"name\": \"" << description.Description
               << L"\",\n    \"dedicated_vram_bytes\": " << description.DedicatedVideoMemory;
        if (budget) {
            output << L",\n    \"local_budget_bytes\": " << budget->local_budget
                   << L",\n    \"local_usage_bytes\": " << budget->local_usage;
        }

        LARGE_INTEGER driver{};
        const bool driverAvailable = SUCCEEDED(adapter3->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
        output << L",\"driver_version_raw\":" << (driverAvailable ? std::to_wstring(driver.QuadPart) : L"null")
            << L"\n  },\"os\":\"Windows\",\"logical_processors\":" << system.dwNumberOfProcessors
            << L",\"ram_physical_bytes\":" << memory.ullTotalPhys << L",\"ram_available_bytes\":" << memory.ullAvailPhys
            << L",\"display_width\":" << display.dmPelsWidth << L",\"display_height\":" << display.dmPelsHeight
            << L",\"display_refresh_hz\":" << display.dmDisplayFrequency << L','
            << std::wstring(benchmarkJson.begin(), benchmarkJson.end())
            << L",\"limitations\":[\"storage cache warmed; not raw disk throughput\",\"GPU workload measurements reported separately\",\"multi-thread timings include thread startup\"]}\n";
        std::cout << "Calibration completed: 8 benchmarks, 9 measured runs each\n";
        return 0;
    }

    std::cerr << "No hardware DXGI adapter available\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
