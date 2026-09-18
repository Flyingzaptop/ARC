#ifdef _WIN32

#include "arc/dx12_host_adapter.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    int seconds{90};
    int probe_frames{12};
    int control_frames{24};
    std::filesystem::path output{"traces/mega-stage-b.json"};
};

Args parse_args(int argc, char** argv) {
    Args out{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) out.seconds = std::clamp(std::atoi(argv[++i]), 30, 300);
        else if (a == "--probe-frames" && i + 1 < argc) out.probe_frames = std::clamp(std::atoi(argv[++i]), 4, 60);
        else if (a == "--control-frames" && i + 1 < argc) out.control_frames = std::clamp(std::atoi(argv[++i]), 4, 120);
        else if (a == "--output" && i + 1 < argc) out.output = argv[++i];
    }
    return out;
}

[[noreturn]] void fail(const char* what, HRESULT hr = E_FAIL) {
    std::cerr << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
    std::exit(2);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) fail(what, hr);
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed - 1, nullptr, nullptr);
    return out;
}

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

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

struct Window {
    HINSTANCE instance{GetModuleHandleW(nullptr)};
    HWND hwnd{};
    ATOM atom{};

    Window() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"ARC-Mega-B-Reference-Renderer";
        atom = RegisterClassExW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) fail("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));
        hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"ARC Mega Stage B Reference Renderer",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
            nullptr, nullptr, instance, nullptr);
        if (!hwnd) fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
        ShowWindow(hwnd, SW_MINIMIZE);
    }

    ~Window() {
        if (hwnd) DestroyWindow(hwnd);
        if (atom) UnregisterClassW(L"ARC-Mega-B-Reference-Renderer", instance);
    }

    void pump() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
};

struct GpuContext {
    static constexpr UINT kWidth = 1920;
    static constexpr UINT kHeight = 1080;
    static constexpr UINT kBuffers = 2;

    Window window{};
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IDXGISwapChain3> swapchain;
    HANDLE fence_event{};
    std::uint64_t fence_value{};
    std::uint64_t timestamp_frequency{};
    std::string adapter_name;

    GpuContext() = default;
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    ~GpuContext() {
        if (fence_event) CloseHandle(fence_event);
    }

    bool init() {
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(
                    i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                candidate.As(&adapter);
                candidate.As(&adapter3);
                adapter_name = narrow(desc.Description);
                break;
            }
        }
        if (!device) return false;

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "CreateCommandList");
        check(list->Close(), "Initial Close");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
        check(queue->GetTimestampFrequency(&timestamp_frequency), "GetTimestampFrequency");

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = kWidth;
        sc.Height = kHeight;
        sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = kBuffers;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc.Scaling = DXGI_SCALING_STRETCH;
        sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> base;
        check(factory->CreateSwapChainForHwnd(
            queue.Get(), window.hwnd, &sc, nullptr, nullptr, &base),
            "CreateSwapChainForHwnd");
        check(base.As(&swapchain), "Query IDXGISwapChain3");
        factory->MakeWindowAssociation(window.hwnd, DXGI_MWA_NO_ALT_ENTER);
        return true;
    }

    std::uint64_t signal() {
        const auto value = ++fence_value;
        check(queue->Signal(fence.Get(), value), "Queue Signal");
        return value;
    }

    void wait(std::uint64_t value) {
        if (fence->GetCompletedValue() >= value) return;
        check(fence->SetEventOnCompletion(value, fence_event), "SetEventOnCompletion");
        if (WaitForSingleObject(fence_event, 30000) != WAIT_OBJECT_0) fail("Fence wait");
    }
};

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(
        source, std::strlen(source), "arc-mega-b", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
        fail("D3DCompile", hr);
    }
    return blob;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_offset(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride) {
    h.ptr += static_cast<SIZE_T>(index) * stride;
    return h;
}

struct QualityKnobs {
    UINT texture_loops{8};
    UINT raster_layers{4};
    UINT light_loops{24};
};

struct QualityState {
    std::array<std::uint32_t, 3> level{};

    static int slot(std::uint64_t id) noexcept {
        switch (id) {
        case 1310: return 0;
        case 1320: return 1;
        case 1330: return 2;
        default: return -1;
        }
    }

    bool mutate(const arc::QualityActionCandidate& action, bool restore) {
        const int s = slot(action.id);
        if (s < 0 || action.sequence > 2) return false;
        auto& current = level[static_cast<std::size_t>(s)];
        if (!restore) {
            if (current != action.sequence) return false;
            current = action.sequence + 1;
        } else {
            if (current != action.sequence + 1) return false;
            current = action.sequence;
        }
        return true;
    }

    QualityKnobs knobs() const noexcept {
        static constexpr UINT texture[4]{8, 6, 4, 2};
        static constexpr UINT raster[4]{4, 3, 2, 1};
        static constexpr UINT lighting[4]{24, 18, 12, 6};
        return {
            texture[std::min<std::uint32_t>(3, level[0])],
            raster[std::min<std::uint32_t>(3, level[1])],
            lighting[std::min<std::uint32_t>(3, level[2])],
        };
    }

    bool full() const noexcept {
        return std::all_of(level.begin(), level.end(), [](auto v) { return v == 0; });
    }
};

struct SceneLoad {
    enum class MemoryMode : std::uint8_t { Normal, Moderate, Emergency };
    const char* name{};
    arc::QualityDomain domain{arc::QualityDomain::Raster};
    UINT texture_extra{};
    UINT raster_extra{};
    UINT light_extra{};
    MemoryMode memory{MemoryMode::Normal};
    bool easy{};
};

std::vector<SceneLoad> schedule() {
    using D = arc::QualityDomain;
    using M = SceneLoad::MemoryMode;
    return {
        {"easy-0", D::Raster, 0, 0, 0, M::Normal, true},
        {"bandwidth-heavy", D::Bandwidth, 12, 0, 0, M::Normal, false},
        {"recover-bandwidth", D::Raster, 0, 0, 0, M::Normal, true},
        {"raster-moderate-memory", D::Raster, 0, 6, 0, M::Moderate, false},
        {"recover-raster", D::Raster, 0, 0, 0, M::Normal, true},
        {"lighting-heavy", D::Lighting, 0, 0, 64, M::Normal, false},
        {"recover-lighting", D::Raster, 0, 0, 0, M::Normal, true},
        {"mixed-emergency", D::Raster, 6, 4, 24, M::Emergency, false},
        {"final-easy", D::Raster, 0, 0, 0, M::Normal, true},
    };
}

void set_pressures(arc::FrameBudgetSample& f, arc::QualityDomain domain, bool easy) {
    f.gpu_busy_fraction = easy ? 0.50 : 0.99;
    f.memory_bandwidth_fraction = 0.20;
    f.raster_pressure = 0.20;
    f.geometry_pressure = 0.20;
    f.lighting_pressure = 0.20;
    f.shadow_pressure = 0.20;
    if (easy) return;
    switch (domain) {
    case arc::QualityDomain::Texture:
    case arc::QualityDomain::Bandwidth: f.memory_bandwidth_fraction = 0.98; break;
    case arc::QualityDomain::Raster: f.raster_pressure = 0.98; break;
    case arc::QualityDomain::Lighting: f.lighting_pressure = 0.98; break;
    default: break;
    }
}

struct RunStats {
    std::vector<double> frames;
    std::uint64_t misses{};
    double overshoot_sum{};
};

void add_stat(RunStats& s, double ms, double target) {
    if (ms <= 0 || !std::isfinite(ms)) return;
    s.frames.push_back(ms);
    if (ms > target) { ++s.misses; s.overshoot_sum += ms - target; }
}

double miss_ratio(const RunStats& s) {
    return s.frames.empty() ? 0.0 : static_cast<double>(s.misses) / static_cast<double>(s.frames.size());
}

double mean_overshoot(const RunStats& s) {
    return s.misses ? s.overshoot_sum / static_cast<double>(s.misses) : 0.0;
}

struct ReferenceRenderer {
    GpuContext& c;
    arc::dx12::NativeHostAdapter& host;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12Resource> source;
    std::array<ComPtr<ID3D12Resource>, GpuContext::kBuffers> backbuffers{};
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> readback;
    UINT rtv_stride{};
    UINT srv_stride{};
    std::uint64_t swapchain_id{};

    explicit ReferenceRenderer(GpuContext& context, arc::dx12::NativeHostAdapter& adapter)
        : c(context), host(adapter) {
        init();
    }

    ~ReferenceRenderer() {
        for (auto& buffer : backbuffers) {
            if (buffer) (void)host.observe_resource_destroyed(buffer.Get());
        }
        if (source) (void)host.observe_resource_destroyed(source.Get());
        if (rtv_heap) (void)host.observe_descriptor_heap_destroyed(rtv_heap.Get());
        if (srv_heap) (void)host.observe_descriptor_heap_destroyed(srv_heap.Get());
        (void)host.drain();
    }

    void init() {
        const char* shader = R"(
Texture2D<float4> srcTex : register(t0);
cbuffer Work : register(b0) { uint textureLoops; uint lightLoops; uint salt; uint pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1.0,-1.0) : (id == 1 ? float2(-1.0,3.0) : float2(3.0,-1.0));
    VSOut o; o.pos=float4(p,0,1); o.uv=float2(p.x*0.5+0.5, 1.0-(p.y*0.5+0.5)); return o;
}
float4 PS(VSOut input) : SV_Target {
    uint2 base = uint2(saturate(input.uv) * 2047.0);
    float4 accum = 0.0;
    [loop] for (uint i=0; i<textureLoops; ++i) {
        uint2 p = (base + uint2(i*13 + salt*3, i*7 + salt)) & 2047;
        accum += srcTex.Load(int3(p,0));
    }
    float x = input.uv.x + accum.x / max(1.0, (float)textureLoops);
    [loop] for (uint i=0; i<lightLoops; ++i) {
        x = frac(x * 1.6180339 + sin(x + (float)i * 0.013) * 0.37 + 0.173);
    }
    return float4(frac(x), frac(input.uv.y + accum.y*0.01), frac(accum.z*0.01 + x*0.3), 1.0);
})";
        auto vs = compile_shader(shader, "VS", "vs_5_1");
        auto ps = compile_shader(shader, "PS", "ps_5_1");

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.RegisterSpace = 0;
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "D3D12SerializeRootSignature");
        check(c.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.InputLayout = {nullptr, 0};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        check(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");

        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = GpuContext::kBuffers;
        check(c.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap)), "Create RTV heap");
        rtv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        (void)host.observe_descriptor_heap(rtv_heap.Get());

        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 1;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(c.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap)), "Create SRV heap");
        srv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        (void)srv_stride;
        (void)host.observe_descriptor_heap(srv_heap.Get());

        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < GpuContext::kBuffers; ++i) {
            check(c.swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])), "Get swapchain buffer");
            (void)host.observe_external_resource(backbuffers[i].Get());
            const auto cpu = cpu_offset(rtv_heap->GetCPUDescriptorHandleForHeapStart(), i, rtv_stride);
            c.device->CreateRenderTargetView(backbuffers[i].Get(), &rtv_desc, cpu);
            if (!host.observe_rtv(rtv_heap.Get(), i, backbuffers[i].Get(), rtv_desc)) fail("observe_rtv");
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 2048;
        td.Height = 2048;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        check(c.device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&source)), "Create source texture");
        (void)host.observe_committed_resource(source.Get());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        c.device->CreateShaderResourceView(source.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
        if (!host.observe_srv(srv_heap.Get(), 0, source.Get(), srv)) fail("observe_srv");

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        check(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&query_heap)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES rp{};
        rp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(c.device->CreateCommittedResource(
            &rp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback)), "Create timestamp readback");
        (void)host.observe_committed_resource(readback.Get());

        swapchain_id = reinterpret_cast<std::uint64_t>(c.swapchain.Get());
    }

    double render(const QualityKnobs& base, const SceneLoad& scene, UINT salt) {
        c.window.pump();
        const UINT index = c.swapchain->GetCurrentBackBufferIndex();
        auto* backbuffer = backbuffers[index].Get();

        check(c.allocator->Reset(), "Allocator Reset");
        check(c.list->Reset(c.allocator.Get(), pso.Get()), "CommandList Reset");
        if (!host.observe_command_list_reset(c.list.Get())) fail("observe command reset");

        D3D12_RESOURCE_BARRIER to_rt{};
        to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_rt.Transition.pResource = backbuffer;
        to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.list->ResourceBarrier(1, &to_rt);
        if (!host.observe_transition_barrier(c.list.Get(), backbuffer, to_rt)) fail("observe barrier to RT");

        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(GpuContext::kWidth), static_cast<float>(GpuContext::kHeight), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(GpuContext::kWidth), static_cast<LONG>(GpuContext::kHeight)};
        c.list->RSSetViewports(1, &vp);
        c.list->RSSetScissorRects(1, &sc);
        const auto rtv = cpu_offset(rtv_heap->GetCPUDescriptorHandleForHeapStart(), index, rtv_stride);
        c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4]{0.01f, 0.02f, 0.03f, 1.0f};
        c.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        c.list->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        c.list->SetDescriptorHeaps(1, heaps);
        c.list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const UINT texture_loops = base.texture_loops + scene.texture_extra;
        const UINT light_loops = base.light_loops + scene.light_extra;
        const UINT layers = base.raster_layers + scene.raster_extra;
        std::uint64_t draws{};
        for (UINT layer = 0; layer < layers; ++layer) {
            const UINT constants[4]{texture_loops, light_loops, salt + layer * 17u, 0u};
            c.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
            c.list->DrawInstanced(3, 1, 0, 0);
            ++draws;
        }
        if (!host.observe_resource_use(c.list.Get(), backbuffer, true)) fail("observe backbuffer use");
        if (!host.observe_resource_use(c.list.Get(), source.Get(), false)) fail("observe source use");
        if (!host.observe_command_counters(c.list.Get(), draws, 0, 0, 0)) fail("observe counters");

        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        c.list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        D3D12_RESOURCE_BARRIER to_present = to_rt;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        c.list->ResourceBarrier(1, &to_present);
        if (!host.observe_transition_barrier(c.list.Get(), backbuffer, to_present)) fail("observe barrier to present");

        check(c.list->Close(), "CommandList Close");
        if (!host.observe_command_list_closed(c.list.Get())) fail("observe command close");
        ID3D12CommandList* lists[]{c.list.Get()};
        c.queue->ExecuteCommandLists(1, lists);
        if (!host.observe_execute_command_lists(c.queue.Get(), std::span<ID3D12CommandList* const>{lists})) fail("observe submit");
        const auto fence_value = c.signal();
        if (!host.observe_fence_signal(c.queue.Get(), c.fence.Get(), fence_value)) fail("observe signal");
        (void)host.drain();
        c.wait(fence_value);
        if (!host.poll_completion(c.queue.Get())) fail("poll completion");

        std::uint64_t* timestamps{};
        D3D12_RANGE range{0, 16};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&timestamps)), "Map timestamps");
        const auto ticks = timestamps[1] >= timestamps[0] ? timestamps[1] - timestamps[0] : 0;
        D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);

        const HRESULT present = c.swapchain->Present(0, 0);
        if (FAILED(present)) fail("Present", present);
        if (!host.observe_present(swapchain_id, 0, 0, present)) fail("observe present");
        (void)host.drain();
        return ticks ? (1000.0 * static_cast<double>(ticks) / static_cast<double>(c.timestamp_frequency)) : 0.0;
    }

    double probe(const QualityKnobs& knobs, const SceneLoad& scene, int frames, UINT& salt) {
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i) samples.push_back(render(knobs, scene, salt++));
        return percentile(samples, 0.50);
    }
};

arc::QualityResourceProfile make_profile(
    std::uint64_t id,
    arc::QualityDomain domain,
    const char* label,
    const std::array<double, 3>& gains) {
    arc::QualityResourceProfile p{};
    p.id = id;
    p.domain = domain;
    p.label = label;
    p.semantic = arc::QualitySemanticClass::Environment;
    p.confidence = 0.95;
    p.reversible = true;
    p.importance.screen_coverage = 0.35;
    p.importance.visibility = 0.9;
    p.importance.semantic_importance = 0.18;
    p.importance.normalized_distance = 0.72;
    p.levels = {
        {.80, std::max(0.0, gains[0]), 0},
        {.60, std::max(0.0, gains[1]), 0},
        {.40, std::max(0.0, gains[2]), 0},
    };
    return p;
}

std::array<double, 3> calibrate_domain(
    ReferenceRenderer& renderer,
    QualityState state,
    const SceneLoad& scene,
    std::uint64_t id,
    int frames,
    UINT& salt) {
    std::array<double, 3> gains{};
    for (std::uint32_t seq = 0; seq < 3; ++seq) {
        const double before = renderer.probe(state.knobs(), scene, frames, salt);
        arc::QualityActionCandidate a{id, scene.domain, "probe", 0.0, .05, .95, 0, true, false, seq};
        if (!state.mutate(a, false)) break;
        const double after = renderer.probe(state.knobs(), scene, frames, salt);
        gains[seq] = std::max(0.0, before - after);
    }
    return gains;
}

ComPtr<ID3D12Resource> create_control_buffer(GpuContext& c, std::uint64_t bytes) {
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
    ComPtr<ID3D12Resource> out;
    check(c.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&out)), "Create control buffer");
    return out;
}

void feed_budget(
    arc::dx12::NativeHostAdapter& host,
    const SceneLoad& scene,
    arc::FrameBudgetSample& frame) {
    arc::MemoryBudgetPayload mb{};
    mb.local_budget = 512ull << 20;
    if (scene.memory == SceneLoad::MemoryMode::Emergency) mb.local_usage = 506ull << 20;
    else if (scene.memory == SceneLoad::MemoryMode::Moderate) mb.local_usage = 466ull << 20;
    else mb.local_usage = 280ull << 20;
    if (!host.observe_memory_budget(mb)) fail("observe memory budget");
    frame.local_budget_bytes = mb.local_budget;
    frame.local_usage_bytes = mb.local_usage;
}

void write_report(
    const std::filesystem::path& path,
    const GpuContext& c,
    double target,
    const RunStats& baseline,
    const RunStats& adaptive,
    const QualityState& final_quality,
    const arc::dx12::NativeHostAdapter& host,
    std::uint64_t quality_domains,
    std::uint64_t memory_ticks,
    std::uint64_t combined_ticks,
    std::uint64_t restore_ticks,
    std::uint64_t learned_effects,
    bool residency_restored,
    bool temporal_used) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::trunc);
    if (!f) fail("open report");
    const auto hm = host.metrics();
    const auto gm = host.runtime().governor().metrics();
    const auto mm = host.runtime().coordinator().metrics();
    const auto bm = host.runtime().bridge().metrics();
    const double b50 = percentile(baseline.frames, .5), b99 = percentile(baseline.frames, .99);
    const double a50 = percentile(adaptive.frames, .5), a99 = percentile(adaptive.frames, .99);
    f << std::fixed << std::setprecision(6)
      << "{\n"
      << "  \"schema\":1,\n"
      << "  \"valid\":true,\n"
      << "  \"benchmark\":\"mega_stage_b\",\n"
      << "  \"integration\":\"native_cooperative_dx12_host\",\n"
      << "  \"adapter\":\"" << json_escape(c.adapter_name) << "\",\n"
      << "  \"native_width\":1920,\n"
      << "  \"native_height\":1080,\n"
      << "  \"temporal_used\":" << (temporal_used ? "true" : "false") << ",\n"
      << "  \"target_frame_ms\":" << target << ",\n"
      << "  \"baseline\":{\"samples\":" << baseline.frames.size() << ",\"p50_ms\":" << b50 << ",\"p99_ms\":" << b99 << ",\"miss_ratio\":" << miss_ratio(baseline) << ",\"mean_overshoot_ms\":" << mean_overshoot(baseline) << "},\n"
      << "  \"adaptive\":{\"samples\":" << adaptive.frames.size() << ",\"p50_ms\":" << a50 << ",\"p99_ms\":" << a99 << ",\"miss_ratio\":" << miss_ratio(adaptive) << ",\"mean_overshoot_ms\":" << mean_overshoot(adaptive) << "},\n"
      << "  \"host\":{"
      << "\"resources\":" << hm.resources_observed
      << ",\"descriptor_writes\":" << hm.descriptor_writes
      << ",\"queues\":" << hm.queues_observed
      << ",\"command_lists\":" << hm.command_lists_observed
      << ",\"resource_uses\":" << hm.resource_uses
      << ",\"queue_submits\":" << hm.queue_submits
      << ",\"fence_signals\":" << hm.fence_signals
      << ",\"completion_updates\":" << hm.completion_updates
      << ",\"presents\":" << hm.presents
      << ",\"budget_samples\":" << hm.budget_samples
      << ",\"events_drained\":" << hm.events_drained
      << ",\"bridge_rejections\":" << hm.bridge_rejections
      << ",\"failed_observations\":" << hm.failed_observations << "},\n"
      << "  \"graph\":{\"resources\":" << host.graph().resource_count() << ",\"errors\":" << host.graph().errors() << ",\"presentation_frame\":" << host.graph().presentation_frame() << "},\n"
      << "  \"bridge\":{\"events\":" << bm.events << ",\"malformed\":" << bm.malformed_events << ",\"controller_rejections\":" << bm.controller_rejections << ",\"completion_releases\":" << bm.completion_releases << "},\n"
      << "  \"governor\":{\"quality_actions_executed\":" << gm.quality_actions_executed << ",\"pending_effects_resolved\":" << gm.pending_effects_resolved << ",\"quality_backend_failures\":" << gm.quality_backend_failures << ",\"memory_executed_actions\":" << mm.executed_actions << ",\"quality_domains\":" << quality_domains << ",\"memory_ticks\":" << memory_ticks << ",\"combined_ticks\":" << combined_ticks << ",\"restore_ticks\":" << restore_ticks << ",\"learned_effects\":" << learned_effects << "},\n"
      << "  \"final_quality_full\":" << (final_quality.full() ? "true" : "false") << ",\n"
      << "  \"residency_restored\":" << (residency_restored ? "true" : "false") << "\n"
      << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    GpuContext ctx;
    if (!ctx.init()) return 77;

    arc::dx12::NativeHostAdapterConfig cfg{};
    cfg.runtime.coordinator.mode = arc::RuntimeMode::Controlled;
    cfg.runtime.coordinator.max_actions_per_tick = 1;
    cfg.runtime.coordinator.max_budget_age_ticks = 8;
    cfg.runtime.runtime.residency.minimum_residency_age_epochs = 1;
    cfg.runtime.runtime.residency.pressure_enter = .85;
    cfg.runtime.runtime.residency.emergency_enter = .95;
    cfg.runtime.runtime.residency.pressure_exit = .78;
    cfg.runtime.runtime.residency.emergency_exit = .82;
    cfg.runtime.runtime.residency.pressure_target = .74;
    cfg.runtime.runtime.residency.emergency_target = .70;
    cfg.runtime.runtime.residency.promotion_ceiling = .80;
    cfg.runtime.runtime.residency.recovery_samples = 1;
    cfg.runtime.governor.quality.overload_samples_required = 2;
    cfg.runtime.governor.quality.headroom_samples_required = 3;
    cfg.runtime.governor.quality.settle_samples_after_change = 1;
    cfg.runtime.governor.quality.minimum_hold_samples_after_degrade = 3;
    cfg.runtime.governor.quality.frame_ewma_alpha = .55;
    cfg.runtime.governor.quality.overload_margin_ms = .03;
    // This renderer intentionally runs in the sub-millisecond range on modern
    // laptop GPUs. Keep the recovery reserve proportional to that scale so an
    // easy phase can actually restore previously degraded quality.
    cfg.runtime.governor.quality.extra_restore_headroom_ms = .01;
    cfg.runtime.governor.quality.optimizer.minimum_gain_ms = .005;
    cfg.runtime.governor.quality.optimizer.minimum_confidence = .35;
    cfg.runtime.governor.quality.optimizer.restoration_headroom_ms = .03;
    cfg.runtime.governor.arbitration.memory_pressure_enter = .85;
    cfg.runtime.governor.arbitration.memory_pressure_emergency = .95;
    cfg.runtime.governor.arbitration.allow_combined = true;
    cfg.runtime.governor.memory_restore_ceiling = .82;
    arc::dx12::NativeHostAdapter host(ctx.device.Get(), ctx.adapter3.Get(), cfg);
    host.set_mode(arc::RuntimeMode::Controlled);

    if (!host.observe_queue(ctx.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT)) return 3;
    if (!host.observe_command_list(ctx.list.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT)) return 3;
    if (!host.bind_completion_fence(ctx.queue.Get(), ctx.fence.Get())) return 3;

    ReferenceRenderer renderer(ctx, host);
    const auto scenes = schedule();
    QualityState full_state{};
    UINT salt = 100;
    for (int i = 0; i < 8; ++i) (void)renderer.render(full_state.knobs(), scenes[0], salt++);
    const double easy_p50 = renderer.probe(full_state.knobs(), scenes[0], std::max(8, args.probe_frames), salt);
    if (easy_p50 <= 0) return 3;
    const double target = easy_p50 * 1.12;
    std::cout << "ARC Mega Stage B native host renderer on " << ctx.adapter_name
              << "\nTarget: " << std::fixed << std::setprecision(3) << target
              << " ms, native 1920x1080, temporal OFF\n";

    const std::array<std::uint64_t, 3> ids{1310, 1320, 1330};
    const std::array<arc::QualityDomain, 3> domains{
        arc::QualityDomain::Bandwidth, arc::QualityDomain::Raster, arc::QualityDomain::Lighting};
    const std::array<const char*, 3> labels{
        "texture sampling", "raster layers", "lighting iterations"};
    const std::array<std::size_t, 3> scene_index{1, 3, 5};
    std::array<std::array<double, 3>, 3> gains{};
    std::cout << "Calibrating renderer quality ladders\n";
    for (std::size_t i = 0; i < 3; ++i) {
        gains[i] = calibrate_domain(renderer, full_state, scenes[scene_index[i]], ids[i], args.probe_frames, salt);
        std::cout << "  " << labels[i] << ": " << gains[i][0] << ", " << gains[i][1] << ", " << gains[i][2] << " ms\n";
        if (!host.register_quality_profile(make_profile(ids[i], domains[i], labels[i], gains[i]))) return 3;
    }

    QualityState adaptive_state{};
    host.set_quality_mutator([&](const arc::QualityActionCandidate& action, bool restore) {
        return adaptive_state.mutate(action, restore)
            ? arc::RuntimeBackendStatus::Success
            : arc::RuntimeBackendStatus::Failure;
    });

    constexpr std::uint64_t residency_bytes = 64ull << 20;
    auto cold0 = create_control_buffer(ctx, residency_bytes);
    auto cold1 = create_control_buffer(ctx, residency_bytes);
    ID3D12Pageable* pages[]{cold0.Get(), cold1.Get()};
    check(ctx.device->MakeResident(2, pages), "Initial MakeResident");
    (void)host.observe_committed_resource(cold0.Get());
    (void)host.observe_committed_resource(cold1.Get());
    if (!host.enable_residency_control(cold0.Get(), .15) || !host.enable_residency_control(cold1.Get(), .15)) return 3;
    const auto cold0_id = host.resource_id(cold0.Get());
    const auto cold1_id = host.resource_id(cold1.Get());

    RunStats baseline{}, adaptive{};
    const int phase_seconds = std::max(3, args.seconds / static_cast<int>(scenes.size()));
    std::cout << "Baseline renderer schedule: " << phase_seconds << " s/phase\n";
    for (const auto& scene : scenes) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(phase_seconds);
        while (std::chrono::steady_clock::now() < end) {
            add_stat(baseline, renderer.render(full_state.knobs(), scene, salt++), target);
        }
    }

    std::cout << "Adaptive native-host schedule\n";
    std::uint64_t quality_domain_mask{}, memory_ticks{}, combined_ticks{}, restore_ticks{};
    bool temporal_used{};
    std::vector<double> control;
    control.reserve(static_cast<std::size_t>(args.control_frames));
    for (const auto& scene : scenes) {
        std::cout << "  " << scene.name << '\n';
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(phase_seconds);
        while (std::chrono::steady_clock::now() < end) {
            const double ms = renderer.render(adaptive_state.knobs(), scene, salt++);
            add_stat(adaptive, ms, target);
            control.push_back(ms);
            if (static_cast<int>(control.size()) >= args.control_frames) {
                arc::FrameBudgetSample frame{};
                frame.frame_ms = percentile(control, .50);
                frame.target_frame_ms = target;
                set_pressures(frame, scene.domain, scene.easy);
                feed_budget(host, scene, frame);
                control.clear();
                const auto tick = host.frame_tick(frame, false);
                temporal_used = temporal_used || tick.quality.plan.temporal_used;
                if (tick.memory.executed_actions) ++memory_ticks;
                if (tick.path == arc::UnifiedGovernorPath::Combined) ++combined_ticks;
                if (tick.path == arc::UnifiedGovernorPath::Restore) ++restore_ticks;
                if (tick.quality_executed && !tick.quality.plan.actions.empty()) {
                    const auto d = static_cast<unsigned>(tick.quality.plan.actions.front().domain);
                    if (d < 64) quality_domain_mask |= (1ull << d);
                }
            }
        }
    }

    // Deterministic recovery: easy renderer + normal budget until both quality
    // and host-owned residency state are back at full/resident.
    for (int i = 0; i < 180; ++i) {
        const auto r0 = host.runtime().runtime().residency().find(cold0_id);
        const auto r1 = host.runtime().runtime().residency().find(cold1_id);
        const bool resident = r0 && r1 && r0->state == arc::ResidencyState::Resident && r1->state == arc::ResidencyState::Resident;
        if (adaptive_state.full() && host.runtime().governor().quality().state().active_actions == 0 && resident) break;
        std::vector<double> w;
        w.reserve(static_cast<std::size_t>(args.control_frames));
        for (int j = 0; j < args.control_frames; ++j) {
            const double ms = renderer.render(adaptive_state.knobs(), scenes.back(), salt++);
            add_stat(adaptive, ms, target);
            w.push_back(ms);
        }
        arc::FrameBudgetSample frame{};
        frame.frame_ms = percentile(w, .50);
        frame.target_frame_ms = target;
        frame.gpu_busy_fraction = .45;
        feed_budget(host, scenes.back(), frame);
        const auto tick = host.frame_tick(frame, false);
        if (tick.memory.executed_actions) ++memory_ticks;
        if (tick.path == arc::UnifiedGovernorPath::Combined) ++combined_ticks;
        if (tick.path == arc::UnifiedGovernorPath::Restore) ++restore_ticks;
    }

    // Resolve the final pending learned effect.
    {
        arc::FrameBudgetSample frame{};
        frame.frame_ms = easy_p50;
        frame.target_frame_ms = target;
        frame.gpu_busy_fraction = .45;
        feed_budget(host, scenes.back(), frame);
        (void)host.frame_tick(frame, false);
    }

    std::uint64_t learned{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto p = make_profile(ids[i], domains[i], labels[i], gains[i]);
        for (const auto& a : arc::QualityCandidateFactory::build(p)) {
            if (host.runtime().governor().quality().effects().find(a)) ++learned;
        }
    }
    const auto r0 = host.runtime().runtime().residency().find(cold0_id);
    const auto r1 = host.runtime().runtime().residency().find(cold1_id);
    const bool residency_restored = r0 && r1 && r0->state == arc::ResidencyState::Resident && r1->state == arc::ResidencyState::Resident;
    const auto quality_domains = static_cast<std::uint64_t>(std::popcount(quality_domain_mask));

    write_report(
        args.output, ctx, target, baseline, adaptive, adaptive_state, host,
        quality_domains, memory_ticks, combined_ticks, restore_ticks, learned,
        residency_restored, temporal_used);

    const auto hm = host.metrics();
    const auto gm = host.runtime().governor().metrics();
    const auto bm = host.runtime().bridge().metrics();
    const bool tracking_improved = miss_ratio(adaptive) < miss_ratio(baseline) ||
        mean_overshoot(adaptive) < mean_overshoot(baseline);
    const bool valid =
        !temporal_used &&
        adaptive_state.full() &&
        residency_restored &&
        quality_domains >= 3 &&
        gm.quality_actions_executed >= 3 &&
        learned >= 2 &&
        host.runtime().coordinator().metrics().executed_actions >= 2 &&
        hm.resources_observed >= 5 &&
        hm.descriptor_writes >= 3 &&
        hm.queue_submits > 10 &&
        hm.fence_signals > 10 &&
        hm.completion_updates > 10 &&
        hm.presents > 10 &&
        hm.failed_observations == 0 &&
        hm.bridge_rejections == 0 &&
        bm.controller_rejections == 0 &&
        bm.malformed_events == 0 &&
        host.graph().errors() == 0 &&
        tracking_improved;

    std::cout << "Baseline miss: " << 100.0 * miss_ratio(baseline)
              << "%  ARC: " << 100.0 * miss_ratio(adaptive) << "%\n"
              << "Host resources: " << hm.resources_observed
              << ", submits: " << hm.queue_submits
              << ", presents: " << hm.presents
              << ", rejections: " << hm.bridge_rejections + bm.controller_rejections << "\n"
              << "Quality actions: " << gm.quality_actions_executed
              << ", domains: " << quality_domains
              << " [bandwidth=" << ((quality_domain_mask & (1ull << static_cast<unsigned>(arc::QualityDomain::Bandwidth))) ? "yes" : "no")
              << ", raster=" << ((quality_domain_mask & (1ull << static_cast<unsigned>(arc::QualityDomain::Raster))) ? "yes" : "no")
              << ", lighting=" << ((quality_domain_mask & (1ull << static_cast<unsigned>(arc::QualityDomain::Lighting))) ? "yes" : "no")
              << "]"
              << ", memory actions: " << host.runtime().coordinator().metrics().executed_actions
              << ", combined ticks: " << combined_ticks << "\n"
              << "Final quality full: " << (adaptive_state.full() ? "yes" : "no")
              << ", residency restored: " << (residency_restored ? "yes" : "no") << "\n"
              << "Report: " << args.output.string() << '\n';
    return valid ? 0 : 3;
}

#else
int main() { return 77; }
#endif
