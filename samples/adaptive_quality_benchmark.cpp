#ifdef _WIN32

#include "arc/adaptive_quality.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
struct Args {
    int seconds{60};
    bool headless{false};
    std::filesystem::path output{"traces/adaptive-quality-benchmark.json"};
};

Args parse_args(int argc, char** argv) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) args.seconds = std::clamp(std::atoi(argv[++i]), 6, 600);
        else if (a == "--output" && i + 1 < argc) args.output = argv[++i];
        else if (a == "--headless") args.headless = true;
    }
    return args;
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::cerr << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
        std::exit(2);
    }
}

struct GpuContext {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    std::uint64_t fence_value{};
    UINT64 timestamp_frequency{};
    std::string adapter_name;

    ~GpuContext() { if (fence_event) CloseHandle(fence_event); }
};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, n - 1)), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
    return out;
}

GpuContext make_context() {
    GpuContext c{};
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&c.factory)), "CreateDXGIFactory2");
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (c.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&c.device)))) {
            candidate.As(&c.adapter);
            candidate.As(&c.adapter3);
            c.adapter_name = narrow(desc.Description);
            break;
        }
    }
    if (!c.device) check(E_FAIL, "No D3D12 adapter");

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    check(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)), "CreateCommandQueue");
    check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&c.allocator)), "CreateCommandAllocator");
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list)), "CreateCommandList");
    c.list->Close();
    check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "CreateFence");
    c.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c.fence_event) check(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");
    check(c.queue->GetTimestampFrequency(&c.timestamp_frequency), "GetTimestampFrequency");
    return c;
}

void wait_gpu(GpuContext& c) {
    const auto value = ++c.fence_value;
    check(c.queue->Signal(c.fence.Get(), value), "Signal");
    if (c.fence->GetCompletedValue() < value) {
        check(c.fence->SetEventOnCompletion(value, c.fence_event), "SetEventOnCompletion");
        WaitForSingleObject(c.fence_event, INFINITE);
    }
}

struct Budget { std::uint64_t usage{}; std::uint64_t budget{}; };
Budget query_budget(GpuContext& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!c.adapter3 || FAILED(c.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return {};
    return {info.CurrentUsage, info.Budget};
}

ComPtr<ID3D12PipelineState> create_pipeline(GpuContext& c, ComPtr<ID3D12RootSignature>& root) {
    static constexpr const char* shader = R"(
RWByteAddressBuffer dataBuffer : register(u0);
cbuffer Params : register(b0) { uint bufferBytes; uint iterations; uint salt; uint padding; };
[numthreads(256,1,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint usable = max(4, bufferBytes & ~3u);
    uint address = (tid.x * 4u) % usable;
    uint v = dataBuffer.Load(address) ^ salt;
    [loop] for (uint i = 0; i < iterations; ++i) {
        v = v * 1664525u + 1013904223u;
        v ^= (v >> 13);
        v = (v << 7) | (v >> 25);
    }
    dataBuffer.Store(address, v);
}
)";

    ComPtr<ID3DBlob> cs, error;
    const HRESULT chr = D3DCompile(shader, std::strlen(shader), "arc-stage6", nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &error);
    if (FAILED(chr)) {
        if (error) std::cerr << static_cast<const char*>(error->GetBufferPointer()) << "\n";
        check(chr, "D3DCompile");
    }

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> rs_blob, rs_error;
    check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_error), "D3D12SerializeRootSignature");
    check(c.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateComputePipelineState");
    return pso;
}

ComPtr<ID3D12Resource> create_uav_buffer(GpuContext& c, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource");
    return resource;
}

struct WorkloadKnobs {
    std::uint64_t working_set_bytes{512ull << 20};
    UINT groups{16384};
    UINT iterations{24};
};

struct Stats {
    std::vector<double> gpu_ms;
    Budget budget_start{};
    Budget budget_peak{};
    Budget budget_end{};
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = p * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return values[lo];
    const double t = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

Stats run_phase(GpuContext& c, ID3D12RootSignature* root, ID3D12PipelineState* pso, WorkloadKnobs knobs, int seconds) {
    Stats out{};
    auto buffer = create_uav_buffer(c, knobs.working_set_bytes);

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Count = 2;
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    ComPtr<ID3D12QueryHeap> queries;
    check(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "CreateQueryHeap");

    D3D12_HEAP_PROPERTIES read_hp{}; read_hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC read_rd{};
    read_rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    read_rd.Width = 16;
    read_rd.Height = 1;
    read_rd.DepthOrArraySize = 1;
    read_rd.MipLevels = 1;
    read_rd.SampleDesc.Count = 1;
    read_rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(c.device->CreateCommittedResource(&read_hp, D3D12_HEAP_FLAG_NONE, &read_rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

    wait_gpu(c);
    out.budget_start = query_budget(c);
    out.budget_peak = out.budget_start;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::uint32_t salt = 1;

    while (std::chrono::steady_clock::now() < deadline) {
        check(c.allocator->Reset(), "Allocator Reset");
        check(c.list->Reset(c.allocator.Get(), pso), "List Reset");
        c.list->SetComputeRootSignature(root);
        c.list->SetComputeRootUnorderedAccessView(0, buffer->GetGPUVirtualAddress());
        const std::uint32_t constants[4] = {
            static_cast<std::uint32_t>(std::min<std::uint64_t>(knobs.working_set_bytes, 0xffffffffull)),
            knobs.iterations,
            salt++,
            0u
        };
        c.list->SetComputeRoot32BitConstants(1, 4, constants, 0);
        c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        c.list->Dispatch(knobs.groups, 1, 1);
        c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        c.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        check(c.list->Close(), "List Close");
        ID3D12CommandList* lists[] = {c.list.Get()};
        c.queue->ExecuteCommandLists(1, lists);
        wait_gpu(c);

        std::uint64_t* timestamps{};
        D3D12_RANGE range{0, 16};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&timestamps)), "Readback Map");
        const auto begin = timestamps[0];
        const auto end = timestamps[1];
        readback->Unmap(0, nullptr);
        if (end > begin && c.timestamp_frequency > 0) {
            out.gpu_ms.push_back(1000.0 * static_cast<double>(end - begin) / static_cast<double>(c.timestamp_frequency));
        }
        const auto b = query_budget(c);
        out.budget_peak.usage = std::max(out.budget_peak.usage, b.usage);
        out.budget_peak.budget = b.budget;
    }
    wait_gpu(c);
    buffer.Reset();
    wait_gpu(c);
    out.budget_end = query_budget(c);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

void write_report(const std::filesystem::path& path, const GpuContext& c, const Stats& base, const Stats& adaptive,
                  const arc::AdaptiveQualityPlan& plan, WorkloadKnobs before, WorkloadKnobs after) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    const double base_p50 = percentile(base.gpu_ms, 0.50);
    const double base_p95 = percentile(base.gpu_ms, 0.95);
    const double base_p99 = percentile(base.gpu_ms, 0.99);
    const double arc_p50 = percentile(adaptive.gpu_ms, 0.50);
    const double arc_p95 = percentile(adaptive.gpu_ms, 0.95);
    const double arc_p99 = percentile(adaptive.gpu_ms, 0.99);
    f << std::fixed << std::setprecision(6);
    f << "{\n";
    f << "  \"schema\": 1,\n";
    f << "  \"valid\": true,\n";
    f << "  \"adapter\": \"" << json_escape(c.adapter_name) << "\",\n";
    f << "  \"temporal_enabled\": false,\n";
    f << "  \"temporal_used\": " << (plan.temporal_used ? "true" : "false") << ",\n";
    f << "  \"bottleneck\": " << static_cast<int>(plan.bottleneck) << ",\n";
    f << "  \"baseline\": {\"samples\": " << base.gpu_ms.size() << ", \"p50_ms\": " << base_p50 << ", \"p95_ms\": " << base_p95 << ", \"p99_ms\": " << base_p99 << ", \"working_set_bytes\": " << before.working_set_bytes << ", \"groups\": " << before.groups << ", \"iterations\": " << before.iterations << ", \"dxgi_peak_usage\": " << base.budget_peak.usage << ", \"dxgi_budget\": " << base.budget_peak.budget << "},\n";
    f << "  \"adaptive\": {\"samples\": " << adaptive.gpu_ms.size() << ", \"p50_ms\": " << arc_p50 << ", \"p95_ms\": " << arc_p95 << ", \"p99_ms\": " << arc_p99 << ", \"working_set_bytes\": " << after.working_set_bytes << ", \"groups\": " << after.groups << ", \"iterations\": " << after.iterations << ", \"dxgi_peak_usage\": " << adaptive.budget_peak.usage << ", \"dxgi_budget\": " << adaptive.budget_peak.budget << "},\n";
    f << "  \"delta\": {\"p50_ms\": " << (arc_p50 - base_p50) << ", \"p95_ms\": " << (arc_p95 - base_p95) << ", \"p99_ms\": " << (arc_p99 - base_p99) << ", \"planned_gain_ms\": " << plan.planned_gain_ms << ", \"visual_cost\": " << plan.estimated_visual_cost << ", \"memory_freed_bytes\": " << plan.planned_memory_freed_bytes << "},\n";
    f << "  \"actions\": [\n";
    for (std::size_t i = 0; i < plan.actions.size(); ++i) {
        const auto& a = plan.actions[i];
        f << "    {\"id\": " << a.id << ", \"domain\": " << static_cast<int>(a.domain) << ", \"label\": \"" << json_escape(a.label) << "\", \"expected_ms_gain\": " << a.expected_ms_gain << ", \"visual_cost\": " << a.visual_cost << ", \"memory_freed_bytes\": " << a.memory_freed_bytes << "}";
        if (i + 1 != plan.actions.size()) f << ',';
        f << "\n";
    }
    f << "  ]\n}\n";
}

void apply_actions(const arc::AdaptiveQualityPlan& plan, WorkloadKnobs& knobs) {
    for (const auto& action : plan.actions) {
        switch (action.id) {
        case 100: knobs.working_set_bytes = std::min<std::uint64_t>(knobs.working_set_bytes, 256ull << 20); break;
        case 101: knobs.working_set_bytes = std::min<std::uint64_t>(knobs.working_set_bytes, 128ull << 20); break;
        case 200: knobs.groups = std::max<UINT>(1024, static_cast<UINT>(static_cast<double>(knobs.groups) * 0.85)); break;
        case 210: knobs.groups = std::max<UINT>(1024, static_cast<UINT>(static_cast<double>(knobs.groups) * 0.85)); break;
        case 300: knobs.iterations = std::max<UINT>(4, knobs.iterations > 6 ? knobs.iterations - 6 : 4); break;
        case 310: knobs.iterations = std::max<UINT>(4, knobs.iterations > 6 ? knobs.iterations - 6 : 4); break;
        default: break;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    auto ctx = make_context();
    ComPtr<ID3D12RootSignature> root;
    auto pso = create_pipeline(ctx, root);

    WorkloadKnobs baseline_knobs{};
    const int phase_seconds = std::max(3, args.seconds / 2);
    std::cout << "ARC Stage 6 benchmark on " << ctx.adapter_name << "\n";
    std::cout << "Phase 1/2 baseline: " << phase_seconds << " s\n";
    auto baseline = run_phase(ctx, root.Get(), pso.Get(), baseline_knobs, phase_seconds);

    const double measured_p50 = percentile(baseline.gpu_ms, 0.50);
    const auto b = baseline.budget_peak;
    arc::FrameBudgetSample sample{};
    sample.frame_ms = measured_p50;
    sample.target_frame_ms = std::max(1.0, measured_p50 * 0.72); // force deterministic deficit for policy exercise
    sample.gpu_busy_fraction = 0.99;
    sample.memory_bandwidth_fraction = 0.93;
    sample.raster_pressure = 0.82;
    sample.geometry_pressure = 0.78;
    sample.lighting_pressure = 0.86;
    sample.local_usage_bytes = b.usage;
    sample.local_budget_bytes = b.budget;

    std::vector<arc::QualityActionCandidate> candidates{
        {100, arc::QualityDomain::Texture, "texture admission 512MiB->256MiB", measured_p50 * 0.05, 0.035, 0.92, 256ull << 20, true, false, 0},
        {101, arc::QualityDomain::Texture, "texture admission 256MiB->128MiB", measured_p50 * 0.035, 0.055, 0.88, 128ull << 20, true, false, 0},
        {200, arc::QualityDomain::Raster, "distant raster workload -15%", measured_p50 * 0.12, 0.045, 0.90, 0, true, false, 0},
        {210, arc::QualityDomain::Geometry, "distant geometry workload -15%", measured_p50 * 0.10, 0.035, 0.88, 0, true, false, 0},
        {300, arc::QualityDomain::Lighting, "far lighting complexity step", measured_p50 * 0.10, 0.050, 0.91, 0, true, false, 0},
        {310, arc::QualityDomain::Shadow, "shadow update complexity step", measured_p50 * 0.08, 0.040, 0.90, 32ull << 20, true, false, 0},
        {900, arc::QualityDomain::Temporal, "temporal upscale (disabled)", measured_p50 * 0.45, 0.010, 0.99, 0, true, true, 0},
    };

    arc::AdaptiveQualityOptimizer optimizer{};
    const auto plan = optimizer.plan_degrade(sample, candidates);
    WorkloadKnobs adaptive_knobs = baseline_knobs;
    apply_actions(plan, adaptive_knobs);

    std::cout << "Selected " << plan.actions.size() << " actions; temporal=" << (plan.temporal_used ? "ON" : "OFF") << "\n";
    for (const auto& action : plan.actions) std::cout << "  - " << action.label << "\n";
    std::cout << "Phase 2/2 adaptive: " << phase_seconds << " s\n";
    auto adaptive = run_phase(ctx, root.Get(), pso.Get(), adaptive_knobs, phase_seconds);
    write_report(args.output, ctx, baseline, adaptive, plan, baseline_knobs, adaptive_knobs);

    const double base_p50 = percentile(baseline.gpu_ms, 0.50);
    const double arc_p50 = percentile(adaptive.gpu_ms, 0.50);
    std::cout << std::fixed << std::setprecision(3)
              << "Baseline P50 GPU: " << base_p50 << " ms\n"
              << "Adaptive P50 GPU: " << arc_p50 << " ms\n"
              << "Delta: " << (arc_p50 - base_p50) << " ms\n"
              << "Report: " << args.output.string() << "\n";
    return (!plan.temporal_used && !baseline.gpu_ms.empty() && !adaptive.gpu_ms.empty()) ? 0 : 3;
}

#else
int main() { return 77; }
#endif
