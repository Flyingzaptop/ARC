#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    DWORD pid{};
    std::wstring game_name{};
    std::filesystem::path out_dir{};
    std::filesystem::path presentmon{};
    std::filesystem::path publish_script{};
    std::filesystem::path repo_root{};
    std::wstring mode{L"external-observe"};
    bool protected_process{};
};

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

std::optional<Args> parse_args(int argc, wchar_t** argv) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view key(argv[i]);
        auto value = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (key == L"--pid") { const auto* v = value(); if (!v) return std::nullopt; args.pid = static_cast<DWORD>(std::wcstoul(v, nullptr, 10)); }
        else if (key == L"--name") { const auto* v = value(); if (!v) return std::nullopt; args.game_name = v; }
        else if (key == L"--out") { const auto* v = value(); if (!v) return std::nullopt; args.out_dir = v; }
        else if (key == L"--presentmon") { const auto* v = value(); if (!v) return std::nullopt; args.presentmon = v; }
        else if (key == L"--publish-script") { const auto* v = value(); if (!v) return std::nullopt; args.publish_script = v; }
        else if (key == L"--repo-root") { const auto* v = value(); if (!v) return std::nullopt; args.repo_root = v; }
        else if (key == L"--mode") { const auto* v = value(); if (!v) return std::nullopt; args.mode = v; }
        else if (key == L"--protected") { const auto* v = value(); if (!v) return std::nullopt; args.protected_process = std::wcstoul(v, nullptr, 10) != 0; }
    }
    if (!args.pid || args.out_dir.empty()) return std::nullopt;
    return args;
}

std::uint64_t filetime_u64(const FILETIME& value) { ULARGE_INTEGER u{}; u.LowPart = value.dwLowDateTime; u.HighPart = value.dwHighDateTime; return u.QuadPart; }

struct AdapterMemoryProbe {
    ComPtr<IDXGIAdapter3> adapter{};
    bool initialize() {
        ComPtr<IDXGIFactory1> factory; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        SIZE_T best_memory{};
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate; if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            ComPtr<IDXGIAdapter3> adapter3; if (FAILED(candidate.As(&adapter3))) continue;
            if (!adapter || desc.DedicatedVideoMemory > best_memory) { adapter = adapter3; best_memory = desc.DedicatedVideoMemory; }
        }
        return adapter != nullptr;
    }
    std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> sample() const {
        if (!adapter) return std::nullopt; DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return std::nullopt; return info;
    }
};

PROCESS_INFORMATION launch_presentmon(const std::filesystem::path& exe, DWORD pid, const std::filesystem::path& output) {
    PROCESS_INFORMATION pi{}; if (exe.empty() || !std::filesystem::exists(exe)) return pi;
    std::wstring command = quote(exe.wstring()) + L" --restart_as_admin --session_name ARC_" + std::to_wstring(pid) +
        L" --process_id " + std::to_wstring(pid) + L" --output_file " + quote(output.wstring()) +
        L" --terminate_on_proc_exit --no_console_stats --qpc_time_ms";
    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return {};
    return pi;
}

DWORD publish_session(const Args& args) {
    if (args.publish_script.empty() || args.repo_root.empty() || !std::filesystem::exists(args.publish_script)) return ERROR_FILE_NOT_FOUND;
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + quote(args.publish_script.wstring()) +
        L" -Session " + quote(args.out_dir.wstring()) + L" -RepoRoot " + quote(args.repo_root.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return GetLastError();
    const auto wait = WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = wait == WAIT_OBJECT_0 ? 0 : WAIT_TIMEOUT;
    if (wait == WAIT_OBJECT_0 && !GetExitCodeProcess(pi.hProcess, &code)) code = GetLastError();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
}

std::vector<std::string> parse_csv_row(const std::string& line) {
    std::vector<std::string> values; std::string current; bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) { const char ch = line[i]; if (ch == '"') { if (quoted && i + 1 < line.size() && line[i + 1] == '"') { current.push_back('"'); ++i; } else quoted = !quoted; } else if (ch == ',' && !quoted) { values.push_back(std::move(current)); current.clear(); } else current.push_back(ch); }
    values.push_back(std::move(current)); return values;
}

std::optional<double> number_at(const std::vector<std::string>& row, std::size_t index) {
    if (index >= row.size() || row[index].empty() || row[index] == "NA") return std::nullopt;
    try { std::size_t consumed{}; const auto value = std::stod(row[index], &consumed); if (consumed != row[index].size() || !std::isfinite(value)) return std::nullopt; return value; } catch (...) { return std::nullopt; }
}

std::optional<std::size_t> column(const std::vector<std::string>& header, std::string_view name) { for (std::size_t i = 0; i < header.size(); ++i) if (header[i] == name) return i; return std::nullopt; }

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0; std::sort(values.begin(), values.end()); const auto position = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(position)); const auto high = static_cast<std::size_t>(std::ceil(position)); const auto fraction = position - static_cast<double>(low);
    return values[low] * (1.0 - fraction) + values[high] * fraction;
}

struct FrameSummary { std::uint64_t frames{}; double average_fps{}, one_percent_low_fps{}, point_one_percent_low_fps{}, p95_frame_ms{}, p99_frame_ms{}, p999_frame_ms{}, median_display_latency_ms{}, p95_display_latency_ms{}, mean_gpu_busy_ms{}; };

FrameSummary summarize_frames(const std::filesystem::path& path) {
    std::ifstream input(path); if (!input) return {}; std::string line; if (!std::getline(input, line)) return {};
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef) line.erase(0, 3);
    const auto header = parse_csv_row(line); const auto displayed = column(header, "DisplayedTime"); const auto between = column(header, "MsBetweenPresents"); const auto latency = column(header, "DisplayLatency"); const auto gpu_busy = column(header, "MsGPUBusy");
    std::vector<double> frame_ms, latencies, gpu_busy_values;
    while (std::getline(input, line)) {
        const auto row = parse_csv_row(line); std::optional<double> frame; if (displayed) frame = number_at(row, *displayed); if ((!frame || *frame <= 0.0) && between) frame = number_at(row, *between);
        if (frame && *frame > 0.0 && *frame < 10000.0) frame_ms.push_back(*frame); if (latency) if (const auto v = number_at(row, *latency); v && *v >= 0.0) latencies.push_back(*v); if (gpu_busy) if (const auto v = number_at(row, *gpu_busy); v && *v >= 0.0) gpu_busy_values.push_back(*v);
    }
    FrameSummary result{}; result.frames = frame_ms.size();
    if (!frame_ms.empty()) { double total{}; for (const auto value : frame_ms) total += value; const auto mean = total / static_cast<double>(frame_ms.size()); result.average_fps = mean > 0.0 ? 1000.0 / mean : 0.0; result.p95_frame_ms = percentile(frame_ms, .95); result.p99_frame_ms = percentile(frame_ms, .99); result.p999_frame_ms = percentile(frame_ms, .999); result.one_percent_low_fps = result.p99_frame_ms > 0.0 ? 1000.0 / result.p99_frame_ms : 0.0; result.point_one_percent_low_fps = result.p999_frame_ms > 0.0 ? 1000.0 / result.p999_frame_ms : 0.0; }
    if (!latencies.empty()) { result.median_display_latency_ms = percentile(latencies, .5); result.p95_display_latency_ms = percentile(latencies, .95); }
    if (!gpu_busy_values.empty()) { double total{}; for (const auto value : gpu_busy_values) total += value; result.mean_gpu_busy_ms = total / static_cast<double>(gpu_busy_values.size()); }
    return result;
}

std::string utf8(const std::wstring& value) { if (value.empty()) return {}; const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr); std::string output(static_cast<std::size_t>(size), '\0'); WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr); return output; }
std::string json_escape(std::string_view text) { std::string out; for (const char ch : text) { switch (ch) { case '\\': out += "\\\\"; break; case '"': out += "\\\""; break; case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break; default: out.push_back(ch); break; } } return out; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const auto args = parse_args(argc, argv); if (!args) return 2;
    std::error_code ec; std::filesystem::create_directories(args->out_dir, ec);
    const auto telemetry_path = args->out_dir / "system-telemetry.csv"; const auto frames_path = args->out_dir / "frames.csv"; const auto summary_path = args->out_dir / "summary.json"; const auto diagnostics_path = args->out_dir / "diagnostics.txt";

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, args->pid); if (!process) return 3;
    HANDLE memory_process = nullptr; if (!args->protected_process) memory_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, args->pid);
    AdapterMemoryProbe gpu; (void)gpu.initialize(); auto presentmon = launch_presentmon(args->presentmon, args->pid, frames_path);
    SYSTEM_INFO system_info{}; GetSystemInfo(&system_info); const auto processors = std::max<DWORD>(1, system_info.dwNumberOfProcessors);
    FILETIME creation{}, exit{}, kernel{}, user{}; GetProcessTimes(process, &creation, &exit, &kernel, &user); auto previous_cpu = filetime_u64(kernel) + filetime_u64(user); auto previous_time = std::chrono::steady_clock::now(); const auto start_time = previous_time;

    std::ofstream telemetry(telemetry_path); telemetry << "elapsed_ms,cpu_total_percent,cpu_one_core_percent,working_set_bytes,private_bytes,dxgi_budget_bytes,dxgi_usage_bytes,dxgi_available_reservation_bytes\n";
    double cpu_sum{}, cpu_peak{}; std::uint64_t cpu_samples{}, working_peak{}, private_peak{}, vram_peak{}, vram_budget_min = UINT64_MAX;
    while (WaitForSingleObject(process, 500) == WAIT_TIMEOUT) {
        const auto now = std::chrono::steady_clock::now(); GetProcessTimes(process, &creation, &exit, &kernel, &user); const auto current_cpu = filetime_u64(kernel) + filetime_u64(user);
        const auto wall_100ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous_time).count()) / 100.0; const auto cpu_100ns = current_cpu >= previous_cpu ? static_cast<double>(current_cpu - previous_cpu) : 0.0;
        const auto one_core = wall_100ns > 0.0 ? 100.0 * cpu_100ns / wall_100ns : 0.0; const auto total_cpu = one_core / static_cast<double>(processors); previous_cpu = current_cpu; previous_time = now;
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); if (memory_process) (void)GetProcessMemoryInfo(memory_process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        const auto gpu_info = gpu.sample(); const auto budget = gpu_info ? gpu_info->Budget : 0; const auto usage = gpu_info ? gpu_info->CurrentUsage : 0; const auto available = gpu_info ? gpu_info->AvailableForReservation : 0; const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        telemetry << elapsed << ',' << std::fixed << std::setprecision(3) << total_cpu << ',' << one_core << ',' << memory.WorkingSetSize << ',' << memory.PrivateUsage << ',' << budget << ',' << usage << ',' << available << '\n'; telemetry.flush();
        cpu_sum += total_cpu; ++cpu_samples; cpu_peak = std::max(cpu_peak, total_cpu); working_peak = std::max<std::uint64_t>(working_peak, memory.WorkingSetSize); private_peak = std::max<std::uint64_t>(private_peak, memory.PrivateUsage); vram_peak = std::max<std::uint64_t>(vram_peak, usage); if (budget) vram_budget_min = std::min<std::uint64_t>(vram_budget_min, budget);
    }

    const auto end_time = std::chrono::steady_clock::now(); DWORD presentmon_exit_code = ERROR_FILE_NOT_FOUND;
    if (presentmon.hProcess) { (void)WaitForSingleObject(presentmon.hProcess, 15000); if (!GetExitCodeProcess(presentmon.hProcess, &presentmon_exit_code)) presentmon_exit_code = GetLastError(); CloseHandle(presentmon.hThread); CloseHandle(presentmon.hProcess); }
    if (memory_process) CloseHandle(memory_process); CloseHandle(process);

    const auto frames = summarize_frames(frames_path); const bool presentmon_available = std::filesystem::exists(args->presentmon); const bool capture_ok = frames.frames > 0;
    if (!capture_ok) { std::ofstream diagnostics(diagnostics_path); diagnostics << "PresentMon capture produced no frame rows.\npresentmon_available=" << presentmon_available << "\npresentmon_exit_code=" << presentmon_exit_code << "\npid=" << args->pid << "\nprotected_process=" << args->protected_process << "\n"; }

    {
        std::ofstream summary(summary_path);
        summary << "{\n  \"schema\": 2,\n  \"mode\": \"" << json_escape(utf8(args->mode)) << "\",\n  \"protected_process\": " << (args->protected_process ? "true" : "false") << ",\n  \"game_name\": \"" << json_escape(utf8(args->game_name)) << "\",\n  \"pid\": " << args->pid << ",\n"
                << "  \"duration_seconds\": " << std::fixed << std::setprecision(3) << std::chrono::duration<double>(end_time - start_time).count() << ",\n  \"presentmon_available\": " << (presentmon_available ? "true" : "false") << ",\n  \"presentmon_exit_code\": " << presentmon_exit_code << ",\n  \"capture_ok\": " << (capture_ok ? "true" : "false") << ",\n"
                << "  \"frames\": " << frames.frames << ",\n  \"average_fps\": " << frames.average_fps << ",\n  \"one_percent_low_fps\": " << frames.one_percent_low_fps << ",\n  \"point_one_percent_low_fps\": " << frames.point_one_percent_low_fps << ",\n  \"p95_frame_ms\": " << frames.p95_frame_ms << ",\n  \"p99_frame_ms\": " << frames.p99_frame_ms << ",\n  \"p999_frame_ms\": " << frames.p999_frame_ms << ",\n"
                << "  \"median_display_latency_ms\": " << frames.median_display_latency_ms << ",\n  \"p95_display_latency_ms\": " << frames.p95_display_latency_ms << ",\n  \"mean_gpu_busy_ms\": " << frames.mean_gpu_busy_ms << ",\n  \"mean_process_cpu_total_percent\": " << (cpu_samples ? cpu_sum / static_cast<double>(cpu_samples) : 0.0) << ",\n  \"peak_process_cpu_total_percent\": " << cpu_peak << ",\n  \"peak_working_set_bytes\": " << working_peak << ",\n  \"peak_private_bytes\": " << private_peak << ",\n  \"peak_dxgi_local_usage_bytes\": " << vram_peak << ",\n  \"minimum_dxgi_local_budget_bytes\": " << (vram_budget_min == UINT64_MAX ? 0 : vram_budget_min) << "\n}\n";
    }

    const DWORD publish_code = publish_session(*args);
    std::ofstream publish_status(args->out_dir / "publish-status.txt"); publish_status << "exit_code=" << publish_code << "\n";
    return capture_ok ? 0 : 4;
}

#else
int main() { return 77; }
#endif
