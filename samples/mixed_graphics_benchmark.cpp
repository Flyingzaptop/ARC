#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "arc/adaptive_quality.hpp"
#include "arc/render_quality_model.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kQueryCount = 10;

enum QueryIndex : UINT {
    ShadowBegin = 0, ShadowEnd,
    GeometryBegin, GeometryEnd,
    RasterBegin, RasterEnd,
    TextureBegin, TextureEnd,
    LightingBegin, LightingEnd,
};

struct Args {
    int seconds{60};
    std::filesystem::path output{"traces/mixed-graphics-benchmark.json"};
};

Args parse_args(int argc, char** argv) {
    Args a{};
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--seconds" && i + 1 < argc) a.seconds = std::clamp(std::atoi(argv[++i]), 6, 600);
        else if (s == "--output" && i + 1 < argc) a.output = argv[++i];
    }
    return a;
}

[[noreturn]] void fail_hr(HRESULT hr, const char* what) {
    std::cerr << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
    std::exit(2);
}
void check(HRESULT hr, const char* what) { if (FAILED(hr)) fail_hr(hr, what); }

struct Context {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    std::uint64_t fence_value{};
    UINT64 timestamp_frequency{};
    std::string adapter_name;

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
    ~Context() { if (event) CloseHandle(event); }
};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
    return out;
}

void init(Context& c) {
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&c.factory)), "CreateDXGIFactory2");
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (c.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        candidate->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&c.device)))) {
            candidate.As(&c.adapter);
            candidate.As(&c.adapter3);
            c.adapter_name = narrow(d.Description);
            break;
        }
    }
    if (!c.device) fail_hr(E_FAIL, "No D3D12 adapter");

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)), "CreateCommandQueue");
    check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator)), "CreateCommandAllocator");
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)), "CreateCommandList");
    check(c.list->Close(), "Initial Close");
    check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "CreateFence");
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c.event) fail_hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
    check(c.queue->GetTimestampFrequency(&c.timestamp_frequency), "GetTimestampFrequency");
}

void wait_gpu(Context& c) {
    const auto value = ++c.fence_value;
    check(c.queue->Signal(c.fence.Get(), value), "Signal");
    if (c.fence->GetCompletedValue() < value) {
        check(c.fence->SetEventOnCompletion(value, c.event), "SetEventOnCompletion");
        if (WaitForSingleObject(c.event, 30000) != WAIT_OBJECT_0) fail_hr(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Fence wait");
    }
}

struct Budget { std::uint64_t usage{}; std::uint64_t budget{}; };
Budget budget(Context& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (!c.adapter3 || FAILED(c.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) return {};
    return {i.CurrentUsage, i.Budget};
}

ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, error;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "arc-stage7", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
    if (FAILED(hr)) {
        if (error) std::cerr << static_cast<const char*>(error->GetBufferPointer()) << "\n";
        fail_hr(hr, entry);
    }
    return blob;
}

struct Pipelines {
    ComPtr<ID3D12RootSignature> simple_root;
    ComPtr<ID3D12RootSignature> texture_root;
    ComPtr<ID3D12PipelineState> geometry;
    ComPtr<ID3D12PipelineState> raster;
    ComPtr<ID3D12PipelineState> texture;
    ComPtr<ID3D12PipelineState> lighting;
};

ComPtr<ID3D12RootSignature> make_simple_root(Context& c) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.ShaderRegister = 0;
    p.Constants.Num32BitValues = 4;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = 1;
    d.pParameters = &p;
    d.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> b, e;
    check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "Serialize simple root");
    ComPtr<ID3D12RootSignature> r;
    check(c.device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&r)), "Create simple root");
    return r;
}

ComPtr<ID3D12RootSignature> make_texture_root(Context& c) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &range;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 0;
    p[1].Constants.Num32BitValues = 4;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.ShaderRegister = 0;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = 2;
    d.pParameters = p;
    d.NumStaticSamplers = 1;
    d.pStaticSamplers = &s;
    d.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> b, e;
    check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "Serialize texture root");
    ComPtr<ID3D12RootSignature> r;
    check(c.device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&r)), "Create texture root");
    return r;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_base(ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.AlphaToCoverageEnable = FALSE;
    d.BlendState.IndependentBlendEnable = FALSE;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kFormat;
    d.SampleDesc.Count = 1;
    return d;
}

Pipelines make_pipelines(Context& c) {
    static constexpr const char* hlsl = R"(
cbuffer Params : register(b0) { uint p0; uint p1; uint p2; uint p3; };
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O fs_vs(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(-1,3) : float2(3,-1));
    O o; o.pos=float4(p,0,1); o.uv=float2((p.x+1)*0.5, 1-(p.y+1)*0.5); return o;
}
O geom_vs(uint id : SV_VertexID, uint inst : SV_InstanceID) {
    uint grid=max(1u,p0); uint x=inst%grid; uint y=inst/grid;
    float2 base=float2((float(x)+0.5)/float(grid)*2-1, (float(y)+0.5)/float(grid)*2-1);
    float2 tri[3]={float2(-0.42,-0.36),float2(0,0.46),float2(0.42,-0.36)};
    float2 q=base+tri[id]*(1.75/float(grid)); O o; o.pos=float4(q,0,1); o.uv=q*0.5+0.5; return o;
}
float4 flat_ps(O i) : SV_Target { return float4(i.uv,0.35,1); }
float4 raster_ps(O i) : SV_Target {
    float3 x=float3(i.uv,0.37); [unroll] for(uint k=0;k<8;k++) x=frac(x*float3(1.71,1.37,1.93)+0.113);
    return float4(x,1);
}
float4 texture_ps(O i) : SV_Target {
    uint n=max(1u,p0); float4 c=0;
    [loop] for(uint k=0;k<n;k++) { float f=float(k+1); float2 off=float2(frac(f*0.618),frac(f*0.414))*0.03125; c+=tex0.SampleLevel(samp0,frac(i.uv+off),0); }
    return c/float(n);
}
float4 lighting_ps(O i) : SV_Target {
    uint n=max(1u,p0); float3 x=float3(i.uv,0.43);
    [loop] for(uint k=0;k<n;k++) { x=frac(x*float3(1.173,1.291,1.337)+float(k)*0.00091); x=sqrt(abs(x)+0.0001); x=frac(x*1.731); }
    return float4(x,1);
}
)";
    auto fs = compile(hlsl, "fs_vs", "vs_5_1");
    auto geom = compile(hlsl, "geom_vs", "vs_5_1");
    auto flat = compile(hlsl, "flat_ps", "ps_5_1");
    auto raster = compile(hlsl, "raster_ps", "ps_5_1");
    auto texture = compile(hlsl, "texture_ps", "ps_5_1");
    auto lighting = compile(hlsl, "lighting_ps", "ps_5_1");

    Pipelines p{};
    p.simple_root = make_simple_root(c);
    p.texture_root = make_texture_root(c);
    auto d = pso_base(p.simple_root.Get(), geom.Get(), flat.Get());
    check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.geometry)), "Geometry PSO");
    d = pso_base(p.simple_root.Get(), fs.Get(), raster.Get());
    check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.raster)), "Raster PSO");
    d = pso_base(p.texture_root.Get(), fs.Get(), texture.Get());
    check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.texture)), "Texture PSO");
    d = pso_base(p.simple_root.Get(), fs.Get(), lighting.Get());
    check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.lighting)), "Lighting PSO");
    return p;
}

ComPtr<ID3D12Resource> make_tex(Context& c, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags,
                                D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE* clear = nullptr) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = kFormat; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, clear, IID_PPV_ARGS(&r)), "Create texture resource");
    return r;
}

struct Knobs {
    UINT texture_dim{4096};
    UINT texture_samples{24};
    UINT geometry_instances{60000};
    UINT raster_layers{7};
    UINT lighting_iterations{48};
    UINT shadow_dim{2048};
    UINT shadow_instances{40000};
};

struct PhaseResources {
    ComPtr<ID3D12Resource> main_rt;
    ComPtr<ID3D12Resource> shadow_rt;
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12DescriptorHeap> srv;
    D3D12_CPU_DESCRIPTOR_HANDLE main_rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_rtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE texture_srv{};
};

PhaseResources make_phase_resources(Context& c, const Knobs& k) {
    PhaseResources r{};
    D3D12_CLEAR_VALUE cv{}; cv.Format = kFormat; cv.Color[3] = 1.0f;
    r.main_rt = make_tex(c, kWidth, kHeight, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    r.shadow_rt = make_tex(c, k.shadow_dim, k.shadow_dim, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    r.texture = make_tex(c, k.texture_dim, k.texture_dim, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 2;
    check(c.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&r.rtv)), "RTV heap");
    r.main_rtv = r.rtv->GetCPUDescriptorHandleForHeapStart();
    r.shadow_rtv = r.main_rtv;
    r.shadow_rtv.ptr += c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    c.device->CreateRenderTargetView(r.main_rt.Get(), nullptr, r.main_rtv);
    c.device->CreateRenderTargetView(r.shadow_rt.Get(), nullptr, r.shadow_rtv);

    D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 1; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(c.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&r.srv)), "SRV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = kFormat; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    c.device->CreateShaderResourceView(r.texture.Get(), &sd, r.srv->GetCPUDescriptorHandleForHeapStart());
    r.texture_srv = r.srv->GetGPUDescriptorHandleForHeapStart();
    return r;
}

struct Series {
    std::vector<double> total, shadow, geometry, raster, texture, lighting;
    Budget start{}, peak{}, end{};
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double x = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(x));
    const auto hi = static_cast<std::size_t>(std::ceil(x));
    if (lo == hi) return v[lo];
    const double t = x - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
}

UINT grid_for(UINT instances) { return static_cast<UINT>(std::ceil(std::sqrt(static_cast<double>(std::max(1u, instances))))); }

Series run_phase(Context& c, const Pipelines& p, const Knobs& k, int seconds) {
    Series s{};
    auto r = make_phase_resources(c, k);
    D3D12_QUERY_HEAP_DESC qd{}; qd.Count = kQueryCount; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    ComPtr<ID3D12QueryHeap> q;
    check(c.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&q)), "Query heap");
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kQueryCount * sizeof(std::uint64_t); rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Query readback");

    wait_gpu(c);
    s.start = budget(c); s.peak = s.start;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    const D3D12_VIEWPORT main_vp{0,0,static_cast<float>(kWidth),static_cast<float>(kHeight),0,1};
    const D3D12_RECT main_sc{0,0,static_cast<LONG>(kWidth),static_cast<LONG>(kHeight)};
    const D3D12_VIEWPORT shadow_vp{0,0,static_cast<float>(k.shadow_dim),static_cast<float>(k.shadow_dim),0,1};
    const D3D12_RECT shadow_sc{0,0,static_cast<LONG>(k.shadow_dim),static_cast<LONG>(k.shadow_dim)};
    const float clear[4]{0.02f,0.025f,0.035f,1.0f};

    while (std::chrono::steady_clock::now() < deadline) {
        check(c.allocator->Reset(), "Allocator Reset");
        check(c.list->Reset(c.allocator.Get(), nullptr), "List Reset");
        c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12DescriptorHeap* heaps[]{r.srv.Get()}; c.list->SetDescriptorHeaps(1, heaps);

        c.list->OMSetRenderTargets(1, &r.shadow_rtv, FALSE, nullptr);
        c.list->RSSetViewports(1, &shadow_vp); c.list->RSSetScissorRects(1, &shadow_sc);
        c.list->ClearRenderTargetView(r.shadow_rtv, clear, 0, nullptr);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ShadowBegin);
        c.list->SetPipelineState(p.geometry.Get()); c.list->SetGraphicsRootSignature(p.simple_root.Get());
        const UINT shconst[4]{grid_for(k.shadow_instances),0,0,0}; c.list->SetGraphicsRoot32BitConstants(0,4,shconst,0);
        c.list->DrawInstanced(3, k.shadow_instances, 0, 0);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ShadowEnd);

        c.list->OMSetRenderTargets(1, &r.main_rtv, FALSE, nullptr);
        c.list->RSSetViewports(1, &main_vp); c.list->RSSetScissorRects(1, &main_sc);
        c.list->ClearRenderTargetView(r.main_rtv, clear, 0, nullptr);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GeometryBegin);
        c.list->SetPipelineState(p.geometry.Get()); c.list->SetGraphicsRootSignature(p.simple_root.Get());
        const UINT gconst[4]{grid_for(k.geometry_instances),0,0,0}; c.list->SetGraphicsRoot32BitConstants(0,4,gconst,0);
        c.list->DrawInstanced(3, k.geometry_instances, 0, 0);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GeometryEnd);

        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RasterBegin);
        c.list->SetPipelineState(p.raster.Get()); c.list->SetGraphicsRootSignature(p.simple_root.Get());
        const UINT rconst[4]{0,0,0,0}; c.list->SetGraphicsRoot32BitConstants(0,4,rconst,0);
        for (UINT i=0;i<k.raster_layers;++i) c.list->DrawInstanced(3,1,0,0);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RasterEnd);

        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, TextureBegin);
        c.list->SetPipelineState(p.texture.Get()); c.list->SetGraphicsRootSignature(p.texture_root.Get());
        c.list->SetGraphicsRootDescriptorTable(0, r.texture_srv);
        const UINT tconst[4]{k.texture_samples,0,0,0}; c.list->SetGraphicsRoot32BitConstants(1,4,tconst,0);
        c.list->DrawInstanced(3,1,0,0);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, TextureEnd);

        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, LightingBegin);
        c.list->SetPipelineState(p.lighting.Get()); c.list->SetGraphicsRootSignature(p.simple_root.Get());
        const UINT lconst[4]{k.lighting_iterations,0,0,0}; c.list->SetGraphicsRoot32BitConstants(0,4,lconst,0);
        c.list->DrawInstanced(3,1,0,0);
        c.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, LightingEnd);

        c.list->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kQueryCount, readback.Get(), 0);
        check(c.list->Close(), "List Close");
        ID3D12CommandList* lists[]{c.list.Get()}; c.queue->ExecuteCommandLists(1, lists); wait_gpu(c);

        std::uint64_t* ts{}; D3D12_RANGE range{0, kQueryCount*sizeof(std::uint64_t)};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&ts)), "Map timestamps");
        auto ms = [&](UINT a, UINT b) { return (ts[b] > ts[a] && c.timestamp_frequency) ? 1000.0 * double(ts[b]-ts[a]) / double(c.timestamp_frequency) : 0.0; };
        const double sh = ms(ShadowBegin,ShadowEnd), ge = ms(GeometryBegin,GeometryEnd), ra = ms(RasterBegin,RasterEnd), te = ms(TextureBegin,TextureEnd), li = ms(LightingBegin,LightingEnd);
        readback->Unmap(0, nullptr);
        s.shadow.push_back(sh); s.geometry.push_back(ge); s.raster.push_back(ra); s.texture.push_back(te); s.lighting.push_back(li); s.total.push_back(sh+ge+ra+te+li);
        const auto b = budget(c); s.peak.usage = std::max(s.peak.usage,b.usage); s.peak.budget = b.budget;
    }
    wait_gpu(c); r = {}; wait_gpu(c); s.end = budget(c); return s;
}

arc::RenderPassTimings p50_timings(const Series& s) {
    arc::RenderPassTimings t{};
    t.texture_ms=pct(s.texture,.5); t.geometry_ms=pct(s.geometry,.5); t.raster_ms=pct(s.raster,.5); t.lighting_ms=pct(s.lighting,.5); t.shadow_ms=pct(s.shadow,.5); t.total_ms=pct(s.total,.5); return t;
}

void apply(const arc::AdaptiveQualityPlan& plan, Knobs& k) {
    for (const auto& a : plan.actions) {
        switch (a.id) {
        case 1000: k.texture_dim = std::min(k.texture_dim, 2048u); break;
        case 1010: k.texture_samples = std::min(k.texture_samples, 14u); break;
        case 2000: k.geometry_instances = std::min(k.geometry_instances, 42000u); break;
        case 2100: k.raster_layers = std::min(k.raster_layers, 4u); break;
        case 3000: k.lighting_iterations = std::min(k.lighting_iterations, 30u); break;
        case 3100: k.shadow_dim = std::min(k.shadow_dim, 1536u); k.shadow_instances = std::min(k.shadow_instances, 28000u); break;
        default: break;
        }
    }
}

std::string esc(const std::string& s) { std::string o; for(char c:s){ if(c=='\\'||c=='"') o.push_back('\\'); o.push_back(c);} return o; }

void write_stats(std::ofstream& f, const Series& s, const Knobs& k) {
    f << "{\"samples\":" << s.total.size()
      << ",\"p50_ms\":" << pct(s.total,.5) << ",\"p95_ms\":" << pct(s.total,.95) << ",\"p99_ms\":" << pct(s.total,.99)
      << ",\"passes_p50_ms\":{\"shadow\":" << pct(s.shadow,.5) << ",\"geometry\":" << pct(s.geometry,.5) << ",\"raster\":" << pct(s.raster,.5) << ",\"texture\":" << pct(s.texture,.5) << ",\"lighting\":" << pct(s.lighting,.5) << "}"
      << ",\"dxgi_peak_usage\":" << s.peak.usage << ",\"dxgi_budget\":" << s.peak.budget
      << ",\"knobs\":{\"texture_dim\":" << k.texture_dim << ",\"texture_samples\":" << k.texture_samples << ",\"geometry_instances\":" << k.geometry_instances << ",\"raster_layers\":" << k.raster_layers << ",\"lighting_iterations\":" << k.lighting_iterations << ",\"shadow_dim\":" << k.shadow_dim << ",\"shadow_instances\":" << k.shadow_instances << "}}";
}

void write_report(const std::filesystem::path& path, const Context& c, const Series& base, const Series& adaptive,
                  const Knobs& before, const Knobs& after, const arc::AdaptiveQualityPlan& plan) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path); f << std::fixed << std::setprecision(6);
    f << "{\n  \"schema\":1,\n  \"valid\":true,\n  \"adapter\":\"" << esc(c.adapter_name) << "\",\n  \"temporal_enabled\":false,\n  \"temporal_used\":" << (plan.temporal_used?"true":"false") << ",\n  \"bottleneck\":" << static_cast<int>(plan.bottleneck) << ",\n  \"baseline\":"; write_stats(f,base,before);
    f << ",\n  \"adaptive\":"; write_stats(f,adaptive,after);
    f << ",\n  \"delta\":{\"p50_ms\":" << (pct(adaptive.total,.5)-pct(base.total,.5)) << ",\"p99_ms\":" << (pct(adaptive.total,.99)-pct(base.total,.99)) << ",\"planned_gain_ms\":" << plan.planned_gain_ms << ",\"visual_cost\":" << plan.estimated_visual_cost << ",\"memory_freed_bytes\":" << plan.planned_memory_freed_bytes << "},\n  \"actions\":[\n";
    for (std::size_t i=0;i<plan.actions.size();++i) { const auto& a=plan.actions[i]; f << "    {\"id\":"<<a.id<<",\"domain\":"<<static_cast<int>(a.domain)<<",\"label\":\""<<esc(a.label)<<"\",\"expected_ms_gain\":"<<a.expected_ms_gain<<",\"visual_cost\":"<<a.visual_cost<<",\"memory_freed_bytes\":"<<a.memory_freed_bytes<<"}" << (i+1==plan.actions.size()?"":",") << "\n"; }
    f << "  ]\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    Context c; init(c); const auto pipelines = make_pipelines(c);
    const int phase_seconds = std::max(3, args.seconds / 2);
    Knobs base_knobs{};
    std::cout << "ARC Stage 7 mixed graphics benchmark on " << c.adapter_name << "\n";
    std::cout << "Phase 1/2 baseline mixed scene: " << phase_seconds << " s\n";
    auto base = run_phase(c, pipelines, base_knobs, phase_seconds);
    const auto t = p50_timings(base);
    const auto b = base.peak;
    const auto sample = arc::RenderQualityModel::make_frame_sample(t, std::max(0.1, t.total_ms * 0.75), b.usage, b.budget, 0.99);
    const std::vector<arc::RenderQualityStep> steps{
        {1000,arc::QualityDomain::Texture,"texture admission 4096->2048",0.22,0.035,0.88,48ull<<20,true,false,0},
        {1010,arc::QualityDomain::Bandwidth,"texture sampling 24->14",0.34,0.030,0.90,0,true,false,0},
        {2000,arc::QualityDomain::Geometry,"distant geometry instances -30%",0.30,0.035,0.90,0,true,false,0},
        {2100,arc::QualityDomain::Raster,"raster overdraw 7->4",0.38,0.045,0.90,0,true,false,0},
        {3000,arc::QualityDomain::Lighting,"lighting complexity 48->30",0.36,0.050,0.91,0,true,false,0},
        {3100,arc::QualityDomain::Shadow,"shadow 2048->1536 + geometry -30%",0.34,0.045,0.89,7ull<<20,true,false,0},
        {9000,arc::QualityDomain::Temporal,"temporal assist (disabled)",0.55,0.005,1.0,0,true,true,0},
    };
    const auto candidates = arc::RenderQualityModel::build_candidates(t, steps);
    arc::AdaptiveQualityOptimizer optimizer{};
    const auto plan = optimizer.plan_degrade(sample, candidates);
    Knobs adaptive_knobs = base_knobs; apply(plan, adaptive_knobs);
    std::cout << "Selected " << plan.actions.size() << " native actions; temporal=" << (plan.temporal_used?"ON":"OFF") << "\n";
    for (const auto& a:plan.actions) std::cout << "  - " << a.label << "\n";
    std::cout << "Phase 2/2 adaptive mixed scene: " << phase_seconds << " s\n";
    auto adaptive = run_phase(c, pipelines, adaptive_knobs, phase_seconds);
    write_report(args.output,c,base,adaptive,base_knobs,adaptive_knobs,plan);
    std::cout << std::fixed << std::setprecision(3)
              << "Baseline P50: " << pct(base.total,.5) << " ms  Adaptive P50: " << pct(adaptive.total,.5) << " ms  Delta: " << (pct(adaptive.total,.5)-pct(base.total,.5)) << " ms\n"
              << "Pass P50 baseline -> adaptive [ms]\n"
              << "  shadow   " << pct(base.shadow,.5) << " -> " << pct(adaptive.shadow,.5) << "\n"
              << "  geometry " << pct(base.geometry,.5) << " -> " << pct(adaptive.geometry,.5) << "\n"
              << "  raster   " << pct(base.raster,.5) << " -> " << pct(adaptive.raster,.5) << "\n"
              << "  texture  " << pct(base.texture,.5) << " -> " << pct(adaptive.texture,.5) << "\n"
              << "  lighting " << pct(base.lighting,.5) << " -> " << pct(adaptive.lighting,.5) << "\n"
              << "Report: " << args.output.string() << "\n";
    const bool valid = !plan.temporal_used && !plan.actions.empty() && !base.total.empty() && !adaptive.total.empty();
    return valid ? 0 : 3;
}

#else
int main(){return 77;}
#endif
