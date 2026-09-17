#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

struct Stats {
    std::uint64_t samples{};
    std::uint64_t budget_min{UINT64_MAX}, budget_max{};
    long double budget_sum{};
    std::uint64_t usage_min{UINT64_MAX}, usage_max{};
    long double usage_sum{};
    std::uint64_t available_min{UINT64_MAX}, available_max{};
    long double available_sum{};
};

void add(Stats& s, const DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
    ++s.samples;
    s.budget_min = (std::min)(s.budget_min, info.Budget);
    s.budget_max = (std::max)(s.budget_max, info.Budget);
    s.budget_sum += info.Budget;
    s.usage_min = (std::min)(s.usage_min, info.CurrentUsage);
    s.usage_max = (std::max)(s.usage_max, info.CurrentUsage);
    s.usage_sum += info.CurrentUsage;
    s.available_min = (std::min)(s.available_min, info.AvailableForReservation);
    s.available_max = (std::max)(s.available_max, info.AvailableForReservation);
    s.available_sum += info.AvailableForReservation;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring arg_value(int argc, wchar_t** argv, const wchar_t* name, const wchar_t* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    }
    return fallback;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto output = arg_value(argc, argv, L"--output", L"arc-game-monitor.json");
    const auto seconds_text = arg_value(argc, argv, L"--seconds", L"60");
    const auto interval_text = arg_value(argc, argv, L"--interval-ms", L"100");
    const int seconds = (std::max)(1, _wtoi(seconds_text.c_str()));
    const int interval_ms = (std::clamp)(_wtoi(interval_text.c_str()), 20, 5000);

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 2;

    ComPtr<IDXGIAdapter1> adapter1;
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_ADAPTER_DESC1 desc{};
    for (UINT index = 0; ; ++index) {
        adapter1.Reset();
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1)) == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapter1->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(adapter1.As(&adapter3))) break;
    }
    if (!adapter3) return 3;

    Stats stats{};
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) add(stats, info);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    if (!stats.samples) return 4;

    std::filesystem::path output_path(output);
    std::error_code ec;
    if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path(), ec);
    std::ofstream file(output_path, std::ios::binary);
    if (!file) return 5;

    const auto mean = [](long double sum, std::uint64_t n) -> std::uint64_t {
        return n ? static_cast<std::uint64_t>(sum / static_cast<long double>(n)) : 0;
    };
    file << "{\n"
         << "  \"schema\": 1,\n"
         << "  \"adapter\": \"" << narrow(desc.Description) << "\",\n"
         << "  \"seconds\": " << seconds << ",\n"
         << "  \"interval_ms\": " << interval_ms << ",\n"
         << "  \"samples\": " << stats.samples << ",\n"
         << "  \"local_budget_min\": " << stats.budget_min << ",\n"
         << "  \"local_budget_mean\": " << mean(stats.budget_sum, stats.samples) << ",\n"
         << "  \"local_budget_max\": " << stats.budget_max << ",\n"
         << "  \"local_usage_min\": " << stats.usage_min << ",\n"
         << "  \"local_usage_mean\": " << mean(stats.usage_sum, stats.samples) << ",\n"
         << "  \"local_usage_max\": " << stats.usage_max << ",\n"
         << "  \"available_for_reservation_min\": " << stats.available_min << ",\n"
         << "  \"available_for_reservation_mean\": " << mean(stats.available_sum, stats.samples) << ",\n"
         << "  \"available_for_reservation_max\": " << stats.available_max << "\n"
         << "}\n";
    return 0;
}
#else
int main() { return 77; }
#endif
