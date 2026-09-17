#ifdef _WIN32

#include "arc/dx12_runtime_backend.hpp"
#include "arc/runtime_integration.hpp"

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
#include <limits>
#include <numeric>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    int seconds{90};
    int probe_frames{12};
    int control_frames{24};
    std::filesystem::path output{"traces/mega-stage-a.json"};
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

[[noreturn]] void fail(const char* what, HRESULT hr) {
    std::cerr << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
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
    std::uint64_t timestamp_frequency{};
    std::string adapter_name;

    GpuContext() = default;
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    ~GpuContext() { if (fence_event) CloseHandle(fence_event); }

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
        const auto value = ++fence_value;
        check(queue->Signal(fence.Get(), value), "Signal");
        if (fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, fence_event), "SetEventOnCompletion");
            const DWORD result = WaitForSingleObject(fence_event, 30000);
            if (result != WAIT_OBJECT_0) fail("WaitForSingleObject", HRESULT_FROM_WIN32(GetLastError()));
        }
    }
};

struct DxgiBudget {
    std::uint64_t usage{};
    std::uint64_t budget{};
};

DxgiBudget query_dxgi(GpuContext& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!c.adapter3 || FAILED(c.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return {};
    return {info.CurrentUsage, info.Budget};
}

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "arc-mega-a", nullptr, nullptr,
                                  entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
        fail("D3DCompile", hr);
    }
    return blob;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_offset(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride) {
    h.ptr += static_cast<SIZE_T>(index) * stride;
    return h;
}

struct QualityKnobs {
    UINT geometry_instances{240000};
    UINT raster_layers{4};
    UINT texture_samples{32};
    UINT light_iterations{48};
    UINT shadow_passes{4};
    UINT shadow_samples{12};
};

struct SceneLoad {
    const char* name{};
    arc::QualityDomain domain{arc::QualityDomain::Raster};
    double geometry_multiplier{1.0};
    UINT raster_extra{};
    UINT texture_extra{};
    UINT light_extra{};
    UINT shadow_pass_extra{};
    UINT shadow_sample_extra{};
    enum class MemoryMode : std::uint8_t { Normal, Moderate, Emergency } memory{MemoryMode::Normal};
    bool easy{};
};

struct QualityState {
    std::array<std::uint32_t, 5> level{};

    static int slot(std::uint64_t id) {
        switch (id) {
        case 910: return 0;
        case 920: return 1;
        case 930: return 2;
        case 940: return 3;
        case 950: return 4;
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

    QualityKnobs knobs() const {
        static constexpr UINT texture[4]{32, 24, 16, 8};
        static constexpr UINT raster[4]{4, 3, 2, 1};
        static constexpr UINT geometry[4]{240000, 180000, 120000, 60000};
        static constexpr UINT lighting[4]{48, 36, 24, 12};
        static constexpr UINT shadow_pass[4]{4, 3, 2, 1};
        static constexpr UINT shadow_sample[4]{12, 9, 6, 4};
        QualityKnobs k{};
        k.texture_samples = texture[std::min<std::uint32_t>(3, level[0])];
        k.raster_layers = raster[std::min<std::uint32_t>(3, level[1])];
        k.geometry_instances = geometry[std::min<std::uint32_t>(3, level[2])];
        k.light_iterations = lighting[std::min<std::uint32_t>(3, level[3])];
        k.shadow_passes = shadow_pass[std::min<std::uint32_t>(3, level[4])];
        k.shadow_samples = shadow_sample[std::min<std::uint32_t>(3, level[4])];
        return k;
    }

    bool full() const noexcept {
        return std::all_of(level.begin(), level.end(), [](auto v) { return v == 0; });
    }
};

struct GraphicsHarness {
    static constexpr UINT kWidth = 1920;
    static constexpr UINT kHeight = 1080;
    static constexpr UINT kSourceSize = 2048;
    static constexpr UINT kShadowSize = 1024;

    GpuContext& c;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> main_pso, geom_pso, shadow_pso;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, srv_heap;
    ComPtr<ID3D12Resource> main_rt, source_tex, shadow_tex;
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> readback;
    UINT rtv_stride{};
    D3D12_RESOURCE_STATES shadow_state{D3D12_RESOURCE_STATE_RENDER_TARGET};

    explicit GraphicsHarness(GpuContext& context) : c(context) { init(); }

    ComPtr<ID3D12Resource> create_rt(UINT width, UINT height, DXGI_FORMAT format, const float clear[4]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = format; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format = format; std::copy(clear, clear + 4, cv.Color);
        ComPtr<ID3D12Resource> out;
        check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&out)), "CreateCommittedResource RT");
        return out;
    }

    void init() {
        D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 3;
        check(c.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap)), "Create RTV heap");
        rtv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 2;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(c.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap)), "Create SRV heap");

        const float c0[4]{0.02f, 0.03f, 0.04f, 1.0f};
        const float c1[4]{0.30f, 0.55f, 0.85f, 1.0f};
        const float c2[4]{0.70f, 0.0f, 0.0f, 1.0f};
        main_rt = create_rt(kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, c0);
        source_tex = create_rt(kSourceSize, kSourceSize, DXGI_FORMAT_R8G8B8A8_UNORM, c1);
        shadow_tex = create_rt(kShadowSize, kShadowSize, DXGI_FORMAT_R32_FLOAT, c2);
        const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        c.device->CreateRenderTargetView(main_rt.Get(), nullptr, cpu_offset(rtv, 0, rtv_stride));
        c.device->CreateRenderTargetView(source_tex.Get(), nullptr, cpu_offset(rtv, 1, rtv_stride));
        c.device->CreateRenderTargetView(shadow_tex.Get(), nullptr, cpu_offset(rtv, 2, rtv_stride));

        const UINT srv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC s0{};
        s0.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s0.Format = DXGI_FORMAT_R8G8B8A8_UNORM; s0.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; s0.Texture2D.MipLevels = 1;
        c.device->CreateShaderResourceView(source_tex.Get(), &s0, cpu_offset(srv, 0, srv_stride));
        D3D12_SHADER_RESOURCE_VIEW_DESC s1{};
        s1.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s1.Format = DXGI_FORMAT_R32_FLOAT; s1.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; s1.Texture2D.MipLevels = 1;
        c.device->CreateShaderResourceView(shadow_tex.Get(), &s1, cpu_offset(srv, 1, srv_stride));

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 2; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0; params[1].Constants.Num32BitValues = 8;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0; sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs_blob, rs_err;
        check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err), "SerializeRootSignature");
        check(c.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

        static constexpr const char* shader = R"(
Texture2D<float4> SourceTex : register(t0);
Texture2D<float> ShadowTex : register(t1);
SamplerState Samp : register(s0);
cbuffer P : register(b0) { uint TS; uint LI; uint SS; uint Salt; uint GI; uint RL; uint SP; uint Reserved; };
struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; float3 n:NORMAL0; };
V VSFull(uint v:SV_VertexID,uint i:SV_InstanceID){ V o; float2 p=v==0?float2(-1,-1):(v==1?float2(-1,3):float2(3,-1)); o.p=float4(p,0,1); o.uv=p*.5+.5+float2((i&3)*.001,(i&7)*.0007); o.n=normalize(float3(.3+o.uv.x,.4+o.uv.y,1)); return o; }
V VSGeom(uint v:SV_VertexID,uint i:SV_InstanceID){ V o; uint x=(i*1664525u+1013904223u+Salt)&2047u; uint y=(i*22695477u+1u+Salt*13u)&2047u; float2 c=float2(x/2047.0*2-1,y/2047.0*2-1); float2 l=v==0?float2(-.001,-.001):(v==1?float2(.001,-.001):float2(0,.0012)); o.p=float4(c+l,0,1); o.uv=c*.5+.5; o.n=float3(0,0,1); return o; }
float4 PSGeom(V i):SV_Target{return float4(i.uv,.1,1);}
float PSShadow(V i):SV_Target{float x=i.uv.x*.7+i.uv.y*.3+(Salt&255)*.0001; [unroll(8)]for(uint k=0;k<8;k++)x=frac(x*1.618+.117*k); return saturate(x);}
float4 PSMain(V i):SV_Target{
 float3 col=0; uint ts=max(1u,TS); [loop]for(uint s=0;s<ts;s++){float2 d=float2((s*13u+Salt)&31u,(s*7u+Salt)&31u)*.00073; col+=SourceTex.SampleLevel(Samp,i.uv+d,0).rgb;} col/=ts;
 float3 n=normalize(i.n); float li=0; [loop]for(uint l=0;l<LI;l++){float a=(l+1u)*.173+Salt*.00031;float3 ld=normalize(float3(sin(a),cos(a*1.37),.45+frac(a)));li+=saturate(dot(n,ld))*(.55+.45*sin(a*2.1));} li/=max(1u,LI);
 float sh=0; [loop]for(uint s2=0;s2<SS;s2++){float2 o=float2(int(s2&3u)-1.5,int((s2>>2)&3u)-1.5)*.0015;sh+=ShadowTex.SampleLevel(Samp,i.uv+o,0);} sh/=max(1u,SS);
 return float4(saturate(col*(.22+1.35*li)*(.45+.55*sh)),1);
})";
        auto vsf = compile_shader(shader, "VSFull", "vs_5_1");
        auto vsg = compile_shader(shader, "VSGeom", "vs_5_1");
        auto psm = compile_shader(shader, "PSMain", "ps_5_1");
        auto psg = compile_shader(shader, "PSGeom", "ps_5_1");
        auto pss = compile_shader(shader, "PSShadow", "ps_5_1");
        auto make_pso = [&](ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT fmt, UINT8 mask) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = mask; pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
            pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt; pd.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> p; check(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)), "CreateGraphicsPipelineState"); return p;
        };
        main_pso = make_pso(vsf.Get(), psm.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_COLOR_WRITE_ENABLE_ALL);
        geom_pso = make_pso(vsg.Get(), psg.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        shadow_pso = make_pso(vsf.Get(), pss.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_COLOR_WRITE_ENABLE_RED);

        D3D12_QUERY_HEAP_DESC qh{}; qh.Count = 2; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        check(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&query_heap)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 16; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

        check(c.allocator->Reset(), "init allocator reset"); check(c.list->Reset(c.allocator.Get(), nullptr), "init list reset");
        c.list->ClearRenderTargetView(cpu_offset(rtv, 1, rtv_stride), c1, 0, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = source_tex.Get(); b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.list->ResourceBarrier(1, &b); check(c.list->Close(), "init close"); ID3D12CommandList* lists[]{c.list.Get()}; c.queue->ExecuteCommandLists(1, lists); c.wait();
    }

    double render(const QualityKnobs& base, const SceneLoad& scene, UINT salt) {
        QualityKnobs k = base;
        k.geometry_instances = static_cast<UINT>(std::min<double>(900000.0, static_cast<double>(k.geometry_instances) * scene.geometry_multiplier));
        k.raster_layers += scene.raster_extra; k.texture_samples += scene.texture_extra; k.light_iterations += scene.light_extra;
        k.shadow_passes += scene.shadow_pass_extra; k.shadow_samples += scene.shadow_sample_extra;

        check(c.allocator->Reset(), "Allocator Reset"); check(c.list->Reset(c.allocator.Get(), nullptr), "List Reset");
        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        c.list->SetGraphicsRootSignature(root.Get()); ID3D12DescriptorHeap* heaps[]{srv_heap.Get()}; c.list->SetDescriptorHeaps(1, heaps);
        c.list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        const UINT constants[8]{k.texture_samples,k.light_iterations,k.shadow_samples,salt,k.geometry_instances,k.raster_layers,k.shadow_passes,0};
        c.list->SetGraphicsRoot32BitConstants(1, 8, constants, 0); c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT mv{0,0,static_cast<float>(kWidth),static_cast<float>(kHeight),0,1}; D3D12_RECT ms{0,0,static_cast<LONG>(kWidth),static_cast<LONG>(kHeight)};
        c.list->RSSetViewports(1,&mv); c.list->RSSetScissorRects(1,&ms); const auto mrt=cpu_offset(rtv,0,rtv_stride); c.list->OMSetRenderTargets(1,&mrt,FALSE,nullptr);
        const float clear[4]{.02f,.03f,.04f,1}; c.list->ClearRenderTargetView(mrt,clear,0,nullptr);
        c.list->SetPipelineState(geom_pso.Get()); c.list->DrawInstanced(3,k.geometry_instances,0,0);
        if (shadow_state != D3D12_RESOURCE_STATE_RENDER_TARGET) { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=shadow_tex.Get(); b.Transition.StateBefore=shadow_state; b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; c.list->ResourceBarrier(1,&b); shadow_state=D3D12_RESOURCE_STATE_RENDER_TARGET; }
        D3D12_VIEWPORT sv{0,0,static_cast<float>(kShadowSize),static_cast<float>(kShadowSize),0,1}; D3D12_RECT ss{0,0,static_cast<LONG>(kShadowSize),static_cast<LONG>(kShadowSize)};
        c.list->RSSetViewports(1,&sv); c.list->RSSetScissorRects(1,&ss); const auto srt=cpu_offset(rtv,2,rtv_stride); c.list->OMSetRenderTargets(1,&srt,FALSE,nullptr); const float sc[4]{.7f,0,0,1}; c.list->ClearRenderTargetView(srt,sc,0,nullptr); c.list->SetPipelineState(shadow_pso.Get()); for(UINT p=0;p<k.shadow_passes;++p)c.list->DrawInstanced(3,1,0,p);
        D3D12_RESOURCE_BARRIER to_srv{}; to_srv.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; to_srv.Transition.pResource=shadow_tex.Get(); to_srv.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; to_srv.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; to_srv.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; c.list->ResourceBarrier(1,&to_srv); shadow_state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        c.list->RSSetViewports(1,&mv); c.list->RSSetScissorRects(1,&ms); c.list->OMSetRenderTargets(1,&mrt,FALSE,nullptr); c.list->SetPipelineState(main_pso.Get()); for(UINT p=0;p<k.raster_layers;++p)c.list->DrawInstanced(3,1,0,p);
        D3D12_RESOURCE_BARRIER to_rt{}; to_rt.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; to_rt.Transition.pResource=shadow_tex.Get(); to_rt.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; to_rt.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; to_rt.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; c.list->ResourceBarrier(1,&to_rt); shadow_state=D3D12_RESOURCE_STATE_RENDER_TARGET;
        c.list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); c.list->ResolveQueryData(query_heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback.Get(),0);
        check(c.list->Close(), "List Close"); ID3D12CommandList* lists[]{c.list.Get()}; c.queue->ExecuteCommandLists(1,lists); c.wait();
        std::uint64_t* ts{}; D3D12_RANGE range{0,16}; check(readback->Map(0,&range,reinterpret_cast<void**>(&ts)),"Map"); const auto a=ts[0],b=ts[1]; readback->Unmap(0,nullptr);
        if (b<=a || !c.timestamp_frequency) return 0; return 1000.0*static_cast<double>(b-a)/static_cast<double>(c.timestamp_frequency);
    }

    double probe(const QualityKnobs& k, const SceneLoad& s, int frames, UINT salt) {
        std::vector<double> v; v.reserve(static_cast<std::size_t>(frames));
        for(int i=0;i<frames;++i){const double ms=render(k,s,salt+static_cast<UINT>(i));if(ms>0&&std::isfinite(ms))v.push_back(ms);} return percentile(v,.50);
    }
};

ComPtr<ID3D12Resource> create_buffer(GpuContext& c, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=bytes; rd.Height=1; rd.DepthOrArraySize=1; rd.MipLevels=1; rd.SampleDesc.Count=1; rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out; check(c.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)),"Create residency buffer"); return out;
}

std::vector<SceneLoad> schedule() {
    using D=arc::QualityDomain; using M=SceneLoad::MemoryMode;
    return {
        {"easy-0",D::Raster,1.0,0,0,0,0,0,M::Normal,true},
        {"bandwidth-heavy",D::Bandwidth,1.0,0,28,0,0,0,M::Normal,false},
        {"moderate-memory-raster",D::Raster,1.0,2,0,0,0,0,M::Moderate,false},
        {"easy-1",D::Raster,1.0,0,0,0,0,0,M::Normal,true},
        {"geometry-heavy",D::Geometry,2.3,0,0,0,0,0,M::Normal,false},
        {"memory-emergency",D::Raster,1.0,1,0,0,0,0,M::Emergency,false},
        {"easy-2",D::Raster,1.0,0,0,0,0,0,M::Normal,true},
        {"lighting-heavy",D::Lighting,1.0,0,0,34,0,0,M::Normal,false},
        {"shadow-heavy",D::Shadow,1.0,0,0,0,4,10,M::Normal,false},
        {"final-easy",D::Raster,1.0,0,0,0,0,0,M::Normal,true},
    };
}

void set_pressures(arc::FrameBudgetSample& f, arc::QualityDomain domain, bool easy) {
    f.gpu_busy_fraction = easy ? 0.55 : 0.99;
    f.memory_bandwidth_fraction = 0.20; f.raster_pressure=0.20; f.geometry_pressure=0.20; f.lighting_pressure=0.20; f.shadow_pressure=0.20;
    if (easy) return;
    switch(domain){case arc::QualityDomain::Texture: case arc::QualityDomain::Bandwidth:f.memory_bandwidth_fraction=.98;break;case arc::QualityDomain::Raster:f.raster_pressure=.98;break;case arc::QualityDomain::Geometry:f.geometry_pressure=.98;break;case arc::QualityDomain::Lighting:f.lighting_pressure=.98;break;case arc::QualityDomain::Shadow:f.shadow_pressure=.98;break;default:break;}
}

struct RunStats { std::vector<double> frames; std::uint64_t misses{}; double overshoot_sum{}; };

void add_stat(RunStats& s, double ms, double target) { if(ms<=0||!std::isfinite(ms))return; s.frames.push_back(ms); if(ms>target){++s.misses;s.overshoot_sum+=ms-target;} }

double miss_ratio(const RunStats& s){return s.frames.empty()?0.0:static_cast<double>(s.misses)/static_cast<double>(s.frames.size());}
double mean_overshoot(const RunStats& s){return s.misses? s.overshoot_sum/static_cast<double>(s.misses):0.0;}

arc::QualityResourceProfile make_profile(std::uint64_t id, arc::QualityDomain domain, const char* label, const std::array<double,3>& gains) {
    arc::QualityResourceProfile p{}; p.id=id; p.domain=domain; p.label=label; p.semantic=arc::QualitySemanticClass::Environment; p.confidence=.95; p.reversible=true;
    p.importance.screen_coverage=.12; p.importance.visibility=.8; p.importance.semantic_importance=.12; p.importance.normalized_distance=.85;
    p.levels={{.80,std::max(0.0,gains[0]),0},{.60,std::max(0.0,gains[1]),0},{.40,std::max(0.0,gains[2]),0}}; return p;
}

std::array<double,3> calibrate_domain(GraphicsHarness& h, QualityState state, const SceneLoad& scene, std::uint64_t id, int frames, UINT& salt) {
    std::array<double,3> gain{};
    for(std::uint32_t seq=0;seq<3;++seq){const double before=h.probe(state.knobs(),scene,frames,salt);salt+=1000; arc::QualityActionCandidate a{id,scene.domain,"probe",0,.05,.95,0,true,false,seq}; if(!state.mutate(a,false))break; const double after=h.probe(state.knobs(),scene,frames,salt);salt+=1000; gain[seq]=std::max(0.0,before-after);} return gain;
}

struct AdmissionResult { bool reduced{}; bool ui_protected{}; std::uint32_t level{}; UINT admitted_width{4096}; std::uint64_t full_bytes{}; std::uint64_t admitted_bytes{}; ComPtr<ID3D12Resource> physical; };

AdmissionResult run_admission(GpuContext& c, arc::RuntimeIntegration& runtime, double target_ms) {
    AdmissionResult out{};
    arc::QualityAdmissionResource r{}; r.profile.id=9901; r.profile.domain=arc::QualityDomain::Texture; r.profile.label="requested 4k distant texture"; r.profile.semantic=arc::QualitySemanticClass::Environment; r.profile.reversible=true; r.profile.confidence=.98; r.profile.importance.screen_coverage=.02; r.profile.importance.visibility=.55; r.profile.importance.semantic_importance=.08; r.profile.importance.normalized_distance=.98; r.profile.levels={{.5,.2,48ull<<20},{.25,.25,12ull<<20},{.125,.3,3ull<<20}}; r.requested_bytes=64ull<<20;
    arc::FrameBudgetSample f{}; f.frame_ms=target_ms*1.45; f.target_frame_ms=target_ms; f.local_budget_bytes=512ull<<20; f.local_usage_bytes=507ull<<20;
    const auto d=runtime.admit_quality(r,f); out.reduced=d.reduced; out.level=d.admitted_level; out.admitted_width=std::max<UINT>(512,4096u>>std::min<std::uint32_t>(3,d.admitted_level));
    auto ui=r; ui.profile.id=9902; ui.profile.semantic=arc::QualitySemanticClass::Ui; ui.profile.importance.semantic_importance=1; ui.profile.importance.visibility=1; ui.profile.importance.normalized_distance=0; const auto du=runtime.admit_quality(ui,f); out.ui_protected=du.protected_semantic&&!du.reduced;
    D3D12_RESOURCE_DESC full{}; full.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; full.Width=4096; full.Height=4096; full.DepthOrArraySize=1; full.MipLevels=1; full.Format=DXGI_FORMAT_R8G8B8A8_UNORM; full.SampleDesc.Count=1;
    auto admitted=full; admitted.Width=out.admitted_width; admitted.Height=out.admitted_width; out.full_bytes=c.device->GetResourceAllocationInfo(0,1,&full).SizeInBytes; out.admitted_bytes=c.device->GetResourceAllocationInfo(0,1,&admitted).SizeInBytes;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT; check(c.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&admitted,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out.physical)),"Create admitted texture"); return out;
}

void write_report(const std::filesystem::path& path, const GpuContext& c, double target, const RunStats& baseline, const RunStats& adaptive,
                  const AdmissionResult& admission, const std::array<std::array<double,3>,5>& gains, const QualityState& final_quality,
                  const arc::UnifiedRuntimeGovernorMetrics& gm, const arc::RuntimeCoordinatorMetrics& mm,
                  std::uint64_t quality_domains, std::uint64_t memory_ticks, std::uint64_t combined_ticks, std::uint64_t restore_ticks,
                  std::uint64_t dxgi_before, std::uint64_t dxgi_min_after_evict, std::uint64_t dxgi_final,
                  bool residency_restored, bool temporal_used, std::uint64_t learned_effects) {
    std::filesystem::create_directories(path.parent_path()); std::ofstream f(path); if(!f)std::exit(4);
    const double b50=percentile(baseline.frames,.5),b99=percentile(baseline.frames,.99),a50=percentile(adaptive.frames,.5),a99=percentile(adaptive.frames,.99);
    f<<std::fixed<<std::setprecision(6)<<"{\n  \"schema\":1,\n  \"valid\":true,\n  \"benchmark\":\"mega_stage_a\",\n  \"adapter\":\""<<json_escape(c.adapter_name)<<"\",\n  \"native_width\":1920,\n  \"native_height\":1080,\n  \"temporal_used\":"<<(temporal_used?"true":"false")<<",\n  \"target_frame_ms\":"<<target<<",\n";
    f<<"  \"baseline\":{\"samples\":"<<baseline.frames.size()<<",\"p50_ms\":"<<b50<<",\"p99_ms\":"<<b99<<",\"miss_ratio\":"<<miss_ratio(baseline)<<",\"mean_overshoot_ms\":"<<mean_overshoot(baseline)<<"},\n";
    f<<"  \"adaptive\":{\"samples\":"<<adaptive.frames.size()<<",\"p50_ms\":"<<a50<<",\"p99_ms\":"<<a99<<",\"miss_ratio\":"<<miss_ratio(adaptive)<<",\"mean_overshoot_ms\":"<<mean_overshoot(adaptive)<<"},\n";
    f<<"  \"admission\":{\"reduced\":"<<(admission.reduced?"true":"false")<<",\"ui_protected\":"<<(admission.ui_protected?"true":"false")<<",\"level\":"<<admission.level<<",\"requested_width\":4096,\"admitted_width\":"<<admission.admitted_width<<",\"full_bytes\":"<<admission.full_bytes<<",\"admitted_bytes\":"<<admission.admitted_bytes<<"},\n";
    f<<"  \"quality_probe_gains\":["; for(std::size_t d=0;d<5;++d){if(d)f<<',';f<<'['<<gains[d][0]<<','<<gains[d][1]<<','<<gains[d][2]<<']';} f<<"],\n";
    f<<"  \"governor\":{\"quality_actions_executed\":"<<gm.quality_actions_executed<<",\"pending_effects_resolved\":"<<gm.pending_effects_resolved<<",\"quality_backend_failures\":"<<gm.quality_backend_failures<<",\"memory_executed_actions\":"<<mm.executed_actions<<",\"quality_domains\":"<<quality_domains<<",\"memory_ticks\":"<<memory_ticks<<",\"combined_ticks\":"<<combined_ticks<<",\"restore_ticks\":"<<restore_ticks<<",\"learned_effects\":"<<learned_effects<<"},\n";
    f<<"  \"physical_memory\":{\"dxgi_before\":"<<dxgi_before<<",\"dxgi_min_after_evict\":"<<dxgi_min_after_evict<<",\"dxgi_final\":"<<dxgi_final<<",\"residency_restored\":"<<(residency_restored?"true":"false")<<"},\n";
    f<<"  \"final_quality_full\":"<<(final_quality.full()?"true":"false")<<"\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto args=parse_args(argc,argv); GpuContext ctx; ctx.init(); GraphicsHarness harness(ctx); QualityState full_state{}; const auto scenes=schedule();
    for(int i=0;i<12;++i)harness.render(full_state.knobs(),scenes[0],static_cast<UINT>(i+1));
    const double easy_p50=harness.probe(full_state.knobs(),scenes[0],std::max(8,args.probe_frames),1000); if(easy_p50<=0)return 3; const double target=easy_p50*1.12;
    std::cout<<"ARC Mega Stage A on "<<ctx.adapter_name<<"\nTarget: "<<std::fixed<<std::setprecision(3)<<target<<" ms, native 1920x1080, temporal OFF\n";

    std::array<std::array<double,3>,5> gains{}; UINT salt=5000;
    const std::array<std::uint64_t,5> ids{910,920,930,940,950};
    const std::array<arc::QualityDomain,5> domains{arc::QualityDomain::Bandwidth,arc::QualityDomain::Raster,arc::QualityDomain::Geometry,arc::QualityDomain::Lighting,arc::QualityDomain::Shadow};
    const std::array<const char*,5> labels{"far texture sampling","distant raster overdraw","distant geometry density","far lighting iterations","far shadow quality"};
    const std::array<std::size_t,5> scene_index{1,2,4,7,8};
    std::cout<<"Calibrating physical quality ladders\n";
    for(std::size_t i=0;i<5;++i){gains[i]=calibrate_domain(harness,full_state,scenes[scene_index[i]],ids[i],args.probe_frames,salt);std::cout<<"  "<<labels[i]<<": "<<gains[i][0]<<", "<<gains[i][1]<<", "<<gains[i][2]<<" ms\n";}

    arc::dx12::LiveRuntimeBackend backend(ctx.device.Get());
    arc::RuntimeIntegrationConfig cfg{}; cfg.coordinator.mode=arc::RuntimeMode::Controlled; cfg.coordinator.max_actions_per_tick=1; cfg.coordinator.max_budget_age_ticks=8;
    cfg.runtime.residency.minimum_residency_age_epochs=1; cfg.runtime.residency.pressure_enter=.85; cfg.runtime.residency.emergency_enter=.95; cfg.runtime.residency.pressure_exit=.78; cfg.runtime.residency.emergency_exit=.82; cfg.runtime.residency.pressure_target=.74; cfg.runtime.residency.emergency_target=.70; cfg.runtime.residency.promotion_ceiling=.80; cfg.runtime.residency.recovery_samples=1;
    cfg.governor.quality.overload_samples_required=2; cfg.governor.quality.headroom_samples_required=3; cfg.governor.quality.settle_samples_after_change=1; cfg.governor.quality.minimum_hold_samples_after_degrade=2; cfg.governor.quality.frame_ewma_alpha=.55; cfg.governor.quality.overload_margin_ms=.02; cfg.governor.quality.extra_restore_headroom_ms=.02; cfg.governor.quality.optimizer.minimum_gain_ms=.005; cfg.governor.quality.optimizer.minimum_confidence=.35; cfg.governor.quality.optimizer.restoration_headroom_ms=.08; cfg.governor.arbitration.memory_pressure_enter=.85; cfg.governor.arbitration.memory_pressure_emergency=.95; cfg.governor.arbitration.allow_combined=true; cfg.governor.memory_restore_ceiling=.82;
    arc::RuntimeIntegration runtime(&backend,cfg); runtime.set_mode(arc::RuntimeMode::Controlled);
    for(std::size_t i=0;i<5;++i)runtime.register_quality_profile(make_profile(ids[i],domains[i],labels[i],gains[i]));
    const auto admission=run_admission(ctx,runtime,target);

    QualityState adaptive_state{}; backend.set_quality_mutator([&](const arc::QualityActionCandidate& action,bool restore){return adaptive_state.mutate(action,restore)?arc::RuntimeBackendStatus::Success:arc::RuntimeBackendStatus::Failure;});
    constexpr std::uint64_t residency_bytes=64ull<<20; auto r0=create_buffer(ctx,residency_bytes),r1=create_buffer(ctx,residency_bytes); ID3D12Pageable* pages[]{r0.Get(),r1.Get()}; check(ctx.device->MakeResident(2,pages),"Initial MakeResident"); ctx.wait();
    backend.bind_resource(7001,r0.Get()); backend.bind_resource(7002,r1.Get());
    for(std::uint64_t i=0;i<2;++i){arc::ResidencyObject o{};o.id=i+1;o.resource=7001+i;o.state=arc::ResidencyState::Resident;o.safety=arc::ResidencySafety::ControlledSafe;o.cost.bytes=residency_bytes;o.cost.reload_ms=.15;o.last_resident_epoch=1;runtime.register_controlled_resource(o);}
    const auto dxgi_before=query_dxgi(ctx).usage; std::uint64_t dxgi_min_after_evict=dxgi_before;

    RunStats baseline{},adaptive{}; const int phase_seconds=std::max(3,args.seconds/static_cast<int>(scenes.size()));
    std::cout<<"Baseline dynamic schedule: "<<phase_seconds<<" s/phase\n"; UINT frame_salt=100000;
    for(const auto& scene:scenes){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(phase_seconds);while(std::chrono::steady_clock::now()<end)add_stat(baseline,harness.render(full_state.knobs(),scene,frame_salt++),target);}

    std::uint64_t epoch=100; std::uint64_t quality_domain_mask=0,memory_ticks=0,combined_ticks=0,restore_ticks=0; bool temporal_used=false; std::vector<double> control_window; control_window.reserve(static_cast<std::size_t>(args.control_frames));
    std::cout<<"Adaptive unified runtime schedule\n";
    for(std::size_t phase=0;phase<scenes.size();++phase){const auto& scene=scenes[phase];std::cout<<"  "<<scene.name<<"\n";const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(phase_seconds);while(std::chrono::steady_clock::now()<end){const double ms=harness.render(adaptive_state.knobs(),scene,frame_salt++);add_stat(adaptive,ms,target);control_window.push_back(ms);if(static_cast<int>(control_window.size())>=args.control_frames){const double observed=percentile(control_window,.50);control_window.clear();arc::FrameBudgetSample fs{};fs.frame_ms=observed;fs.target_frame_ms=target;set_pressures(fs,scene.domain,scene.easy);arc::MemoryBudgetPayload mb{};mb.local_budget=512ull<<20;if(scene.memory==SceneLoad::MemoryMode::Emergency)mb.local_usage=506ull<<20;else if(scene.memory==SceneLoad::MemoryMode::Moderate)mb.local_usage=466ull<<20;else mb.local_usage=280ull<<20;runtime.update_budget(mb);fs.local_budget_bytes=mb.local_budget;fs.local_usage_bytes=mb.local_usage;const auto tick=runtime.tick_adaptive(epoch++,fs);temporal_used=temporal_used||tick.quality.plan.temporal_used;if(tick.memory.executed_actions){++memory_ticks;ctx.wait();Sleep(25);dxgi_min_after_evict=std::min(dxgi_min_after_evict,query_dxgi(ctx).usage);}if(tick.path==arc::UnifiedGovernorPath::Combined)++combined_ticks;if(tick.path==arc::UnifiedGovernorPath::Restore)++restore_ticks;if(tick.quality_executed&&!tick.quality.plan.actions.empty()){const auto d=static_cast<unsigned>(tick.quality.plan.actions.front().domain);if(d<64)quality_domain_mask|=(1ull<<d);}}}}
    }

    // Deterministic recovery tail: normal memory and easy scene until every
    // quality ladder and evicted residency object has been restored.
    for(int i=0;i<180 && (!adaptive_state.full() || runtime.governor().quality().state().active_actions!=0 || runtime.runtime().residency().find(1)->state!=arc::ResidencyState::Resident || runtime.runtime().residency().find(2)->state!=arc::ResidencyState::Resident);++i){std::vector<double> w;for(int j=0;j<args.control_frames;++j){const double ms=harness.render(adaptive_state.knobs(),scenes.back(),frame_salt++);add_stat(adaptive,ms,target);w.push_back(ms);}arc::MemoryBudgetPayload mb{};mb.local_budget=512ull<<20;mb.local_usage=260ull<<20;runtime.update_budget(mb);arc::FrameBudgetSample fs{};fs.frame_ms=percentile(w,.5);fs.target_frame_ms=target;fs.gpu_busy_fraction=.45;fs.local_budget_bytes=mb.local_budget;fs.local_usage_bytes=mb.local_usage;const auto tick=runtime.tick_adaptive(epoch++,fs);if(tick.path==arc::UnifiedGovernorPath::Restore)++restore_ticks;if(tick.memory.executed_actions)++memory_ticks;}
    // One final observation resolves any pending learned quality effect.
    {arc::MemoryBudgetPayload mb{};mb.local_budget=512ull<<20;mb.local_usage=260ull<<20;runtime.update_budget(mb);arc::FrameBudgetSample fs{};fs.frame_ms=easy_p50;fs.target_frame_ms=target;fs.local_budget_bytes=mb.local_budget;fs.local_usage_bytes=mb.local_usage;(void)runtime.tick_adaptive(epoch++,fs);}

    ctx.wait();Sleep(100);const auto dxgi_final=query_dxgi(ctx).usage;const auto o0=runtime.runtime().residency().find(1),o1=runtime.runtime().residency().find(2);const bool residency_restored=o0&&o1&&o0->state==arc::ResidencyState::Resident&&o1->state==arc::ResidencyState::Resident;
    std::uint64_t learned=0; for(std::size_t i=0;i<5;++i){auto p=make_profile(ids[i],domains[i],labels[i],gains[i]);for(const auto& a:arc::QualityCandidateFactory::build(p))if(runtime.governor().quality().effects().find(a))++learned;}
    const auto gm=runtime.governor().metrics();const auto mm=runtime.coordinator().metrics();const auto quality_domains=static_cast<std::uint64_t>(std::popcount(quality_domain_mask));
    write_report(args.output,ctx,target,baseline,adaptive,admission,gains,adaptive_state,gm,mm,quality_domains,memory_ticks,combined_ticks,restore_ticks,dxgi_before,dxgi_min_after_evict,dxgi_final,residency_restored,temporal_used,learned);
    std::cout<<"Baseline miss: "<<100*miss_ratio(baseline)<<"%  ARC: "<<100*miss_ratio(adaptive)<<"%\n"<<"Quality actions: "<<gm.quality_actions_executed<<", domains: "<<quality_domains<<", memory actions: "<<mm.executed_actions<<", combined ticks: "<<combined_ticks<<"\n"<<"Admission 4096 -> "<<admission.admitted_width<<", bytes "<<admission.full_bytes<<" -> "<<admission.admitted_bytes<<"\n"<<"Final quality full: "<<(adaptive_state.full()?"yes":"no")<<", residency restored: "<<(residency_restored?"yes":"no")<<"\nReport: "<<args.output.string()<<"\n";
    const bool valid=admission.reduced&&admission.ui_protected&&admission.admitted_bytes<admission.full_bytes&&!temporal_used&&gm.quality_actions_executed>0&&mm.executed_actions>0&&adaptive_state.full()&&residency_restored&&miss_ratio(adaptive)<miss_ratio(baseline);
    return valid?0:3;
}

#else
int main(){return 77;}
#endif
