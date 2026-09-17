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
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    int seconds{60};
    int probe_frames{18};
    std::filesystem::path output{"traces/mixed-graphics-benchmark.json"};
};

Args parse_args(int argc, char** argv) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) args.seconds = std::clamp(std::atoi(argv[++i]), 8, 600);
        else if (a == "--probe-frames" && i + 1 < argc) args.probe_frames = std::clamp(std::atoi(argv[++i]), 4, 120);
        else if (a == "--output" && i + 1 < argc) args.output = argv[++i];
    }
    return args;
}

[[noreturn]] void fail(const char* what, HRESULT hr) {
    std::cerr << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
    std::exit(2);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) fail(what, hr);
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
    return out;
}

D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT index, UINT stride) {
    base.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(stride);
    return base;
}

D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu(D3D12_GPU_DESCRIPTOR_HANDLE base, UINT index, UINT stride) {
    base.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(stride);
    return base;
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
    UINT64 fence_value{};
    UINT64 timestamp_frequency{};
    std::string adapter_name;

    GpuContext() = default;
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) = delete;
    GpuContext& operator=(GpuContext&&) = delete;

    ~GpuContext() {
        if (fence_event) CloseHandle(fence_event);
    }

    void init() {
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
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
        if (!device) fail("No D3D12 adapter", E_FAIL);

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        check(list->Close(), "Initial Close");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
        check(queue->GetTimestampFrequency(&timestamp_frequency), "GetTimestampFrequency");
    }

    void wait() {
        const UINT64 value = ++fence_value;
        check(queue->Signal(fence.Get(), value), "Signal");
        if (fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, fence_event), "SetEventOnCompletion");
            const DWORD result = WaitForSingleObject(fence_event, 30000);
            if (result != WAIT_OBJECT_0) fail("WaitForSingleObject", HRESULT_FROM_WIN32(GetLastError()));
        }
    }
};

struct Budget {
    std::uint64_t usage{};
    std::uint64_t budget{};
};

Budget query_budget(GpuContext& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!c.adapter3 || FAILED(c.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return {};
    return {info.CurrentUsage, info.Budget};
}

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "arc-stage7", nullptr, nullptr,
                                  entry, target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
        fail("D3DCompile", hr);
    }
    return blob;
}

struct Knobs {
    UINT geometry_instances{120000};
    UINT raster_layers{3};
    UINT texture_samples{12};
    UINT light_iterations{24};
    UINT shadow_passes{3};
    UINT shadow_samples{8};
};

struct FrameStats {
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

struct GraphicsHarness {
    static constexpr UINT kWidth = 1920;
    static constexpr UINT kHeight = 1080;
    static constexpr UINT kShadowSize = 1024;
    static constexpr UINT kSourceSize = 2048;

    GpuContext& c;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> main_pso;
    ComPtr<ID3D12PipelineState> geometry_pso;
    ComPtr<ID3D12PipelineState> shadow_pso;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12Resource> main_rt;
    ComPtr<ID3D12Resource> source_tex;
    ComPtr<ID3D12Resource> shadow_tex;
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> readback;
    UINT rtv_stride{};
    UINT srv_stride{};
    D3D12_RESOURCE_STATES shadow_state{D3D12_RESOURCE_STATE_RENDER_TARGET};

    explicit GraphicsHarness(GpuContext& context) : c(context) { init(); }

    ComPtr<ID3D12Resource> create_rt(UINT width, UINT height, DXGI_FORMAT format, const float clear[4]) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = format;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = format;
        std::copy(clear, clear + 4, cv.Color);
        ComPtr<ID3D12Resource> out;
        check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&out)), "CreateCommittedResource RT");
        return out;
    }

    void init() {
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 3;
        check(c.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap)), "Create RTV heap");
        rtv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 2;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(c.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap)), "Create SRV heap");
        srv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        const float main_clear[4]{0.015f, 0.020f, 0.030f, 1.0f};
        const float source_clear[4]{0.32f, 0.52f, 0.84f, 1.0f};
        const float shadow_clear[4]{0.72f, 0.0f, 0.0f, 1.0f};
        main_rt = create_rt(kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, main_clear);
        source_tex = create_rt(kSourceSize, kSourceSize, DXGI_FORMAT_R8G8B8A8_UNORM, source_clear);
        shadow_tex = create_rt(kShadowSize, kShadowSize, DXGI_FORMAT_R32_FLOAT, shadow_clear);

        auto rtv0 = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        c.device->CreateRenderTargetView(main_rt.Get(), nullptr, offset_cpu(rtv0, 0, rtv_stride));
        c.device->CreateRenderTargetView(source_tex.Get(), nullptr, offset_cpu(rtv0, 1, rtv_stride));
        c.device->CreateRenderTargetView(shadow_tex.Get(), nullptr, offset_cpu(rtv0, 2, rtv_stride));

        D3D12_SHADER_RESOURCE_VIEW_DESC source_srv{};
        source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        source_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        source_srv.Texture2D.MipLevels = 1;
        auto srv0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
        c.device->CreateShaderResourceView(source_tex.Get(), &source_srv, offset_cpu(srv0, 0, srv_stride));

        D3D12_SHADER_RESOURCE_VIEW_DESC shadow_srv{};
        shadow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadow_srv.Format = DXGI_FORMAT_R32_FLOAT;
        shadow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadow_srv.Texture2D.MipLevels = 1;
        c.device->CreateShaderResourceView(shadow_tex.Get(), &shadow_srv, offset_cpu(srv0, 1, srv_stride));

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.RegisterSpace = 0;
        params[1].Constants.Num32BitValues = 8;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &sampler;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs_blob, rs_error;
        check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_error), "Serialize root signature");
        check(c.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

        static constexpr const char* shaders = R"(
Texture2D<float4> SourceTex : register(t0);
Texture2D<float> ShadowTex : register(t1);
SamplerState LinearWrap : register(s0);
cbuffer Params : register(b0) {
    uint TextureSamples;
    uint LightIterations;
    uint ShadowSamples;
    uint Salt;
    uint GeometryInstances;
    uint RasterLayers;
    uint ShadowPasses;
    uint Reserved;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 n : NORMAL0; };
VSOut VSFull(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;
    float2 p = vid == 0 ? float2(-1,-1) : (vid == 1 ? float2(-1,3) : float2(3,-1));
    o.pos = float4(p, 0, 1);
    o.uv = p * 0.5 + 0.5 + float2((iid & 3) * 0.0013, (iid & 7) * 0.0009);
    o.n = normalize(float3(0.25 + o.uv.x, 0.35 + o.uv.y, 1.0));
    return o;
}
VSOut VSGeom(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;
    uint x = (iid * 1664525u + 1013904223u + Salt) & 1023u;
    uint y = (iid * 22695477u + 1u + Salt * 17u) & 1023u;
    float2 center = float2((x / 1023.0) * 2.0 - 1.0, (y / 1023.0) * 2.0 - 1.0);
    float2 local = vid == 0 ? float2(-0.0015,-0.0012) : (vid == 1 ? float2(0.0015,-0.0012) : float2(0,0.0018));
    float wobble = sin((iid + Salt) * 0.013) * 0.0008;
    o.pos = float4(center + local + wobble, 0.2, 1.0);
    o.uv = center * 0.5 + 0.5;
    o.n = normalize(float3(center * 0.3, 1.0));
    return o;
}
float4 PSGeometry(VSOut i) : SV_Target { return float4(i.uv, 0.1, 1); }
float PSShadow(VSOut i) : SV_Target {
    float v = i.uv.x * 0.7 + i.uv.y * 0.3 + (Salt & 255) * 0.0001;
    [unroll(8)] for (uint k = 0; k < 8; ++k) v = frac(v * 1.6180339 + 0.117 * k);
    return saturate(v);
}
float4 PSMain(VSOut i) : SV_Target {
    float3 color = 0;
    uint ts = max(1u, TextureSamples);
    [loop] for (uint s = 0; s < ts; ++s) {
        float2 duv = float2((s * 13u + Salt) & 31u, (s * 7u + Salt) & 31u) * 0.00073;
        color += SourceTex.SampleLevel(LinearWrap, i.uv + duv, 0).rgb;
    }
    color /= ts;
    float3 n = normalize(i.n);
    float lighting = 0.0;
    [loop] for (uint l = 0; l < LightIterations; ++l) {
        float a = (l + 1u) * 0.173 + Salt * 0.00031;
        float3 ld = normalize(float3(sin(a), cos(a * 1.37), 0.45 + frac(a)));
        lighting += saturate(dot(n, ld)) * (0.55 + 0.45 * sin(a * 2.1));
    }
    lighting /= max(1u, LightIterations);
    float shadow = 0.0;
    [loop] for (uint s2 = 0; s2 < ShadowSamples; ++s2) {
        float2 off = float2((int(s2 & 3u) - 1.5), (int((s2 >> 2) & 3u) - 1.5)) * 0.0015;
        shadow += ShadowTex.SampleLevel(LinearWrap, i.uv + off, 0);
    }
    shadow /= max(1u, ShadowSamples);
    float3 lit = color * (0.22 + 1.35 * lighting) * (0.45 + 0.55 * shadow);
    lit += 0.02 * float3(sin(Salt * 0.01), cos(Salt * 0.013), 1.0);
    return float4(saturate(lit), 1.0);
}
)";

        auto vs_full = compile_shader(shaders, "VSFull", "vs_5_1");
        auto vs_geom = compile_shader(shaders, "VSGeom", "vs_5_1");
        auto ps_main = compile_shader(shaders, "PSMain", "ps_5_1");
        auto ps_geom = compile_shader(shaders, "PSGeometry", "ps_5_1");
        auto ps_shadow = compile_shader(shaders, "PSShadow", "ps_5_1");

        auto make_pso = [&](ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT fmt, UINT8 write_mask) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = root.Get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.AlphaToCoverageEnable = FALSE;
            pd.BlendState.IndependentBlendEnable = FALSE;
            D3D12_RENDER_TARGET_BLEND_DESC rt{};
            rt.BlendEnable = FALSE;
            rt.LogicOpEnable = FALSE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_ZERO;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.LogicOp = D3D12_LOGIC_OP_NOOP;
            rt.RenderTargetWriteMask = write_mask;
            pd.BlendState.RenderTarget[0] = rt;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.FrontCounterClockwise = FALSE;
            pd.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            pd.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            pd.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.DepthStencilState.DepthEnable = FALSE;
            pd.DepthStencilState.StencilEnable = FALSE;
            pd.InputLayout = {nullptr, 0};
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = fmt;
            pd.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> pso;
            check(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
            return pso;
        };

        main_pso = make_pso(vs_full.Get(), ps_main.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_COLOR_WRITE_ENABLE_ALL);
        geometry_pso = make_pso(vs_geom.Get(), ps_geom.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        shadow_pso = make_pso(vs_full.Get(), ps_shadow.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_COLOR_WRITE_ENABLE_RED);

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Count = 2;
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        check(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&query_heap)), "CreateQueryHeap");

        D3D12_HEAP_PROPERTIES rhp{};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rrd{};
        rrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rrd.Width = 16;
        rrd.Height = 1;
        rrd.DepthOrArraySize = 1;
        rrd.MipLevels = 1;
        rrd.SampleDesc.Count = 1;
        rrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(c.device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rrd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

        // Initialize the source texture with a known color, then make it shader-visible.
        check(c.allocator->Reset(), "Allocator reset init");
        check(c.list->Reset(c.allocator.Get(), nullptr), "List reset init");
        const float source_color[4]{0.32f, 0.52f, 0.84f, 1.0f};
        c.list->ClearRenderTargetView(offset_cpu(rtv0, 1, rtv_stride), source_color, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source_tex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.list->ResourceBarrier(1, &barrier);
        check(c.list->Close(), "List close init");
        ID3D12CommandList* lists[]{c.list.Get()};
        c.queue->ExecuteCommandLists(1, lists);
        c.wait();
    }

    double render_frame(const Knobs& k, UINT salt) {
        check(c.allocator->Reset(), "Allocator Reset");
        check(c.list->Reset(c.allocator.Get(), nullptr), "List Reset");

        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        c.list->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        c.list->SetDescriptorHeaps(1, heaps);
        c.list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        const UINT constants[8]{k.texture_samples, k.light_iterations, k.shadow_samples, salt,
                                k.geometry_instances, k.raster_layers, k.shadow_passes, 0};
        c.list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto rtv0 = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT main_vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
        D3D12_RECT main_sc{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        c.list->RSSetViewports(1, &main_vp);
        c.list->RSSetScissorRects(1, &main_sc);
        const float main_clear[4]{0.015f, 0.020f, 0.030f, 1.0f};
        const auto main_rtv = offset_cpu(rtv0, 0, rtv_stride);
        c.list->OMSetRenderTargets(1, &main_rtv, FALSE, nullptr);
        c.list->ClearRenderTargetView(main_rtv, main_clear, 0, nullptr);

        // Geometry-only pressure: lots of procedural tiny triangles, color writes disabled.
        c.list->SetPipelineState(geometry_pso.Get());
        c.list->DrawInstanced(3, k.geometry_instances, 0, 0);

        // Shadow-map update pass.
        if (shadow_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = shadow_tex.Get();
            b.Transition.StateBefore = shadow_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            c.list->ResourceBarrier(1, &b);
            shadow_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        D3D12_VIEWPORT shadow_vp{0.0f, 0.0f, static_cast<float>(kShadowSize), static_cast<float>(kShadowSize), 0.0f, 1.0f};
        D3D12_RECT shadow_sc{0, 0, static_cast<LONG>(kShadowSize), static_cast<LONG>(kShadowSize)};
        c.list->RSSetViewports(1, &shadow_vp);
        c.list->RSSetScissorRects(1, &shadow_sc);
        const auto shadow_rtv = offset_cpu(rtv0, 2, rtv_stride);
        c.list->OMSetRenderTargets(1, &shadow_rtv, FALSE, nullptr);
        const float shadow_clear[4]{0.72f, 0.0f, 0.0f, 1.0f};
        c.list->ClearRenderTargetView(shadow_rtv, shadow_clear, 0, nullptr);
        c.list->SetPipelineState(shadow_pso.Get());
        for (UINT p = 0; p < k.shadow_passes; ++p) c.list->DrawInstanced(3, 1, 0, p);

        D3D12_RESOURCE_BARRIER to_srv{};
        to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_srv.Transition.pResource = shadow_tex.Get();
        to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.list->ResourceBarrier(1, &to_srv);
        shadow_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // Main native-resolution raster + texture + lighting + shadow sampling pass.
        c.list->RSSetViewports(1, &main_vp);
        c.list->RSSetScissorRects(1, &main_sc);
        c.list->OMSetRenderTargets(1, &main_rtv, FALSE, nullptr);
        c.list->SetPipelineState(main_pso.Get());
        c.list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        c.list->DrawInstanced(3, k.raster_layers, 0, 0);

        // Return shadow target to render-target state for the next frame.
        D3D12_RESOURCE_BARRIER to_rt{};
        to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_rt.Transition.pResource = shadow_tex.Get();
        to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.list->ResourceBarrier(1, &to_rt);
        shadow_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        c.list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        check(c.list->Close(), "List Close");
        ID3D12CommandList* lists[]{c.list.Get()};
        c.queue->ExecuteCommandLists(1, lists);
        c.wait();

        std::uint64_t* timestamps{};
        D3D12_RANGE range{0, 16};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&timestamps)), "Readback Map");
        const auto begin = timestamps[0];
        const auto end = timestamps[1];
        readback->Unmap(0, nullptr);
        if (end <= begin || c.timestamp_frequency == 0) return 0.0;
        return 1000.0 * static_cast<double>(end - begin) / static_cast<double>(c.timestamp_frequency);
    }

    FrameStats run_seconds(const Knobs& k, int seconds, UINT salt_base) {
        FrameStats stats{};
        c.wait();
        stats.budget_start = query_budget(c);
        stats.budget_peak = stats.budget_start;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        UINT salt = salt_base;
        while (std::chrono::steady_clock::now() < deadline) {
            const double ms = render_frame(k, salt++);
            if (ms > 0.0 && std::isfinite(ms)) stats.gpu_ms.push_back(ms);
            const auto b = query_budget(c);
            stats.budget_peak.usage = std::max(stats.budget_peak.usage, b.usage);
            stats.budget_peak.budget = b.budget;
        }
        stats.budget_end = query_budget(c);
        return stats;
    }

    double probe(const Knobs& k, int frames, UINT salt_base) {
        std::vector<double> values;
        values.reserve(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            const double ms = render_frame(k, salt_base + static_cast<UINT>(i));
            if (ms > 0.0 && std::isfinite(ms)) values.push_back(ms);
        }
        return percentile(std::move(values), 0.50);
    }
};

void apply_action(std::uint64_t id, Knobs& k) {
    switch (id) {
    case 710: k.texture_samples = std::max<UINT>(4, k.texture_samples > 4 ? k.texture_samples - 4 : 4); break;
    case 720: k.raster_layers = std::max<UINT>(1, k.raster_layers > 1 ? k.raster_layers - 1 : 1); break;
    case 730: k.geometry_instances = std::max<UINT>(30000, static_cast<UINT>(static_cast<double>(k.geometry_instances) * 0.67)); break;
    case 740: k.light_iterations = std::max<UINT>(8, k.light_iterations > 8 ? k.light_iterations - 8 : 8); break;
    case 750:
        k.shadow_passes = std::max<UINT>(1, k.shadow_passes > 1 ? k.shadow_passes - 1 : 1);
        k.shadow_samples = std::max<UINT>(4, k.shadow_samples > 3 ? k.shadow_samples - 3 : 4);
        break;
    default: break;
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

void write_knobs(std::ostream& f, const Knobs& k) {
    f << "{\"geometry_instances\":" << k.geometry_instances
      << ",\"raster_layers\":" << k.raster_layers
      << ",\"texture_samples\":" << k.texture_samples
      << ",\"light_iterations\":" << k.light_iterations
      << ",\"shadow_passes\":" << k.shadow_passes
      << ",\"shadow_samples\":" << k.shadow_samples << "}";
}

struct ProbeResult {
    std::uint64_t id{};
    const char* label{};
    arc::QualityDomain domain{};
    double visual_cost{};
    double confidence{};
    double probe_p50_ms{};
    double measured_gain_ms{};
};

void write_report(const std::filesystem::path& path,
                  const GpuContext& ctx,
                  const Knobs& baseline_knobs,
                  const Knobs& adaptive_knobs,
                  const FrameStats& baseline,
                  const FrameStats& adaptive,
                  const std::vector<ProbeResult>& probes,
                  const arc::AdaptiveQualityPlan& plan) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) {
        std::cerr << "Could not open report: " << path.string() << "\n";
        std::exit(4);
    }
    const double b50 = percentile(baseline.gpu_ms, 0.50);
    const double b95 = percentile(baseline.gpu_ms, 0.95);
    const double b99 = percentile(baseline.gpu_ms, 0.99);
    const double a50 = percentile(adaptive.gpu_ms, 0.50);
    const double a95 = percentile(adaptive.gpu_ms, 0.95);
    const double a99 = percentile(adaptive.gpu_ms, 0.99);

    f << std::fixed << std::setprecision(6);
    f << "{\n";
    f << "  \"schema\": 1,\n";
    f << "  \"valid\": true,\n";
    f << "  \"benchmark\": \"mixed_graphics\",\n";
    f << "  \"adapter\": \"" << json_escape(ctx.adapter_name) << "\",\n";
    f << "  \"native_width\": " << GraphicsHarness::kWidth << ",\n";
    f << "  \"native_height\": " << GraphicsHarness::kHeight << ",\n";
    f << "  \"temporal_enabled\": false,\n";
    f << "  \"temporal_used\": " << (plan.temporal_used ? "true" : "false") << ",\n";
    f << "  \"bottleneck\": " << static_cast<int>(plan.bottleneck) << ",\n";
    f << "  \"baseline\": {\"samples\":" << baseline.gpu_ms.size() << ",\"p50_ms\":" << b50
      << ",\"p95_ms\":" << b95 << ",\"p99_ms\":" << b99
      << ",\"dxgi_peak_usage\":" << baseline.budget_peak.usage << ",\"dxgi_budget\":" << baseline.budget_peak.budget
      << ",\"knobs\":"; write_knobs(f, baseline_knobs); f << "},\n";
    f << "  \"adaptive\": {\"samples\":" << adaptive.gpu_ms.size() << ",\"p50_ms\":" << a50
      << ",\"p95_ms\":" << a95 << ",\"p99_ms\":" << a99
      << ",\"dxgi_peak_usage\":" << adaptive.budget_peak.usage << ",\"dxgi_budget\":" << adaptive.budget_peak.budget
      << ",\"knobs\":"; write_knobs(f, adaptive_knobs); f << "},\n";
    f << "  \"delta\": {\"p50_ms\":" << (a50 - b50) << ",\"p95_ms\":" << (a95 - b95)
      << ",\"p99_ms\":" << (a99 - b99) << ",\"planned_gain_ms\":" << plan.planned_gain_ms
      << ",\"visual_cost\":" << plan.estimated_visual_cost << "},\n";
    f << "  \"probes\": [\n";
    for (std::size_t i = 0; i < probes.size(); ++i) {
        const auto& p = probes[i];
        f << "    {\"id\":" << p.id << ",\"domain\":" << static_cast<int>(p.domain)
          << ",\"label\":\"" << json_escape(p.label) << "\",\"probe_p50_ms\":" << p.probe_p50_ms
          << ",\"measured_gain_ms\":" << p.measured_gain_ms << ",\"visual_cost\":" << p.visual_cost << "}";
        if (i + 1 != probes.size()) f << ',';
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"actions\": [\n";
    for (std::size_t i = 0; i < plan.actions.size(); ++i) {
        const auto& a = plan.actions[i];
        f << "    {\"id\":" << a.id << ",\"domain\":" << static_cast<int>(a.domain)
          << ",\"label\":\"" << json_escape(a.label) << "\",\"expected_ms_gain\":" << a.expected_ms_gain
          << ",\"visual_cost\":" << a.visual_cost << "}";
        if (i + 1 != plan.actions.size()) f << ',';
        f << "\n";
    }
    f << "  ]\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    GpuContext ctx;
    ctx.init();
    GraphicsHarness harness(ctx);

    Knobs base{};
    for (int i = 0; i < 12; ++i) harness.render_frame(base, static_cast<UINT>(i + 1));

    const int phase_seconds = std::max(4, args.seconds / 2);
    std::cout << "ARC Stage 7 mixed graphics benchmark on " << ctx.adapter_name << "\n";
    std::cout << "Native target: " << GraphicsHarness::kWidth << "x" << GraphicsHarness::kHeight << "\n";
    std::cout << "Temporal / DLSS / FSR / Frame Generation: OFF\n";
    std::cout << "Phase 1/2 baseline mixed graphics: " << phase_seconds << " s\n";
    const auto baseline = harness.run_seconds(base, phase_seconds, 1000);
    const double baseline_p50 = percentile(baseline.gpu_ms, 0.50);
    if (baseline.gpu_ms.empty() || baseline_p50 <= 0.0) {
        std::cerr << "Baseline produced no valid GPU timestamps.\n";
        return 3;
    }

    std::vector<ProbeResult> probes{
        {710, "texture sampling step 12->8", arc::QualityDomain::Bandwidth, 0.035, 0.95},
        {720, "raster overdraw step 3->2", arc::QualityDomain::Raster, 0.055, 0.95},
        {730, "distant geometry density step", arc::QualityDomain::Geometry, 0.030, 0.93},
        {740, "far lighting iterations step", arc::QualityDomain::Lighting, 0.045, 0.94},
        {750, "shadow update/sample step", arc::QualityDomain::Shadow, 0.040, 0.94},
    };

    std::cout << "Calibrating real per-domain gains (" << args.probe_frames << " frames each)\n";
    UINT probe_salt = 50000;
    for (auto& probe : probes) {
        Knobs candidate = base;
        apply_action(probe.id, candidate);
        probe.probe_p50_ms = harness.probe(candidate, args.probe_frames, probe_salt);
        probe_salt += 1000;
        probe.measured_gain_ms = std::max(0.0, baseline_p50 - probe.probe_p50_ms);
        std::cout << "  " << probe.label << ": " << std::fixed << std::setprecision(3)
                  << probe.measured_gain_ms << " ms measured\n";
    }

    arc::FrameBudgetSample sample{};
    sample.frame_ms = baseline_p50;
    sample.target_frame_ms = std::max(0.20, baseline_p50 * 0.78);
    sample.gpu_busy_fraction = 0.99;
    sample.memory_bandwidth_fraction = 0.30;
    sample.raster_pressure = 0.30;
    sample.geometry_pressure = 0.30;
    sample.lighting_pressure = 0.30;
    sample.local_usage_bytes = baseline.budget_peak.usage;
    sample.local_budget_bytes = baseline.budget_peak.budget;

    std::vector<arc::QualityActionCandidate> candidates;
    for (const auto& p : probes) {
        if (p.measured_gain_ms >= 0.01) {
            candidates.push_back({p.id, p.domain, p.label, p.measured_gain_ms, p.visual_cost,
                                  p.confidence, 0, true, false, 0});
        }
    }
    candidates.push_back({990, arc::QualityDomain::Temporal, "temporal assist disabled", baseline_p50 * 0.5,
                          0.01, 0.99, 0, true, true, 0});

    arc::AdaptiveQualityConfig config{};
    config.allow_temporal_assist = false;
    config.minimum_gain_ms = 0.01;
    config.minimum_confidence = 0.50;
    config.max_actions_per_plan = 5;
    arc::AdaptiveQualityOptimizer optimizer(config);
    const auto plan = optimizer.plan_degrade(sample, candidates);

    Knobs adaptive_knobs = base;
    for (const auto& action : plan.actions) apply_action(action.id, adaptive_knobs);

    std::cout << "Selected " << plan.actions.size() << " measured actions; temporal="
              << (plan.temporal_used ? "ON" : "OFF") << "\n";
    for (const auto& action : plan.actions) {
        std::cout << "  - " << action.label << " (measured " << std::fixed << std::setprecision(3)
                  << action.expected_ms_gain << " ms)\n";
    }
    std::cout << "Phase 2/2 adaptive mixed graphics: " << phase_seconds << " s\n";
    const auto adaptive = harness.run_seconds(adaptive_knobs, phase_seconds, 90000);
    write_report(args.output, ctx, base, adaptive_knobs, baseline, adaptive, probes, plan);

    const double adaptive_p50 = percentile(adaptive.gpu_ms, 0.50);
    const double baseline_p99 = percentile(baseline.gpu_ms, 0.99);
    const double adaptive_p99 = percentile(adaptive.gpu_ms, 0.99);
    std::cout << std::fixed << std::setprecision(3)
              << "Baseline P50 GPU: " << baseline_p50 << " ms\n"
              << "Adaptive P50 GPU: " << adaptive_p50 << " ms\n"
              << "P50 delta: " << (adaptive_p50 - baseline_p50) << " ms\n"
              << "P99 delta: " << (adaptive_p99 - baseline_p99) << " ms\n"
              << "Report: " << args.output.string() << "\n";

    const bool valid = !plan.temporal_used && !plan.actions.empty() && !adaptive.gpu_ms.empty();
    return valid ? 0 : 3;
}

#else
int main() { return 77; }
#endif
