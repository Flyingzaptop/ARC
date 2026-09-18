#ifdef _WIN32

#include "arc/temporal_visibility.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

void hr(HRESULT value, const char* what) {
    if (FAILED(value)) {
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(value));
    }
}

ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(
        source, std::strlen(source), "mega-e-visibility-native", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result)) {
        if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
        hr(result, "D3DCompile");
    }
    return code;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(
    ID3D12DescriptorHeap* heap,
    UINT index,
    UINT stride) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * stride;
    return handle;
}

}  // namespace

int main() try {
    constexpr UINT kWidth = 64;
    constexpr UINT kHeight = 64;

    ComPtr<IDXGIFactory6> factory;
    hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(
                i,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(
                candidate.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device)))) {
            adapter = candidate;
            break;
        }
    }
    if (!device) {
        std::cerr << "No native D3D12 hardware\n";
        return 77;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    ComPtr<ID3D12CommandAllocator> allocator;
    hr(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");

    ComPtr<ID3D12GraphicsCommandList> list;
    hr(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&list)), "CreateCommandList");

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)), "Create RTV heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    hr(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)), "Create DSV heap");

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC color_desc{};
    color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color_desc.Width = kWidth;
    color_desc.Height = kHeight;
    color_desc.DepthOrArraySize = 1;
    color_desc.MipLevels = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.SampleDesc.Count = 1;
    color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE color_clear{};
    color_clear.Format = color_desc.Format;
    color_clear.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> color;
    hr(device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &color_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &color_clear,
        IID_PPV_ARGS(&color)), "Create color");

    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = color_desc.Format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(), &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;

    ComPtr<ID3D12Resource> depth;
    hr(device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear,
        IID_PPV_ARGS(&depth)), "Create depth");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth.Get(), &dsv_desc, dsv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameter.Constants.Num32BitValues = 1;
    parameter.Constants.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_errors;
    hr(D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &root_blob,
        &root_errors), "SerializeRootSignature");

    ComPtr<ID3D12RootSignature> root;
    hr(device->CreateRootSignature(
        0,
        root_blob->GetBufferPointer(),
        root_blob->GetBufferSize(),
        IID_PPV_ARGS(&root)), "CreateRootSignature");

    static constexpr char shader[] = R"(
cbuffer Params : register(b0) { float depth_value; };

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    return float4(p[id], depth_value, 1.0);
}

float4 PSMain() : SV_Target {
    return float4(0.8, 0.4, 0.2, 1.0);
}
)";

    const auto vs = compile(shader, "VSMain", "vs_5_0");
    const auto ps = compile(shader, "PSMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.InputLayout = {nullptr, 0};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    hr(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");

    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    query_desc.Count = 2;
    ComPtr<ID3D12QueryHeap> queries;
    hr(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)), "CreateQueryHeap");

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = 2 * sizeof(std::uint64_t);
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    hr(device->CreateCommittedResource(
        &readback_heap,
        D3D12_HEAP_FLAG_NONE,
        &readback_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readback)), "Create readback");

    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();

    list->SetGraphicsRootSignature(root.Get());
    list->SetPipelineState(pso.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const float near_depth = 0.2f;
    list->SetGraphicsRoot32BitConstant(0, std::bit_cast<std::uint32_t>(near_depth), 0);
    list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);

    const float far_depth = 0.8f;
    list->SetGraphicsRoot32BitConstant(0, std::bit_cast<std::uint32_t>(far_depth), 0);
    list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);

    list->ResolveQueryData(
        queries.Get(),
        D3D12_QUERY_TYPE_OCCLUSION,
        0,
        2,
        readback.Get(),
        0);

    hr(list->Close(), "Close");
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent failed");
    hr(queue->Signal(fence.Get(), 1), "Signal");
    hr(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    const auto wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");

    void* mapped = nullptr;
    D3D12_RANGE range{0, 2 * sizeof(std::uint64_t)};
    hr(readback->Map(0, &range, &mapped), "Map");
    const auto* counts = static_cast<const std::uint64_t*>(mapped);
    const std::array<std::uint64_t, 2> samples{counts[0], counts[1]};
    D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);

    const auto visible = arc::visible_coverage_from_occlusion(
        samples[0],
        static_cast<std::uint64_t>(kWidth) * kHeight,
        1);
    const auto hidden = arc::visible_coverage_from_occlusion(
        samples[1],
        static_cast<std::uint64_t>(kWidth) * kHeight,
        1);

    if (!visible || !hidden) throw std::runtime_error("coverage conversion failed");
    if (*visible < 0.90) throw std::runtime_error("visible draw did not cover expected target");
    if (*hidden > 0.001) throw std::runtime_error("occluded draw unexpectedly passed depth");

    arc::TemporalVisibilityModel model;

    arc::VisibilityObservation shown{};
    shown.id = 1;
    shown.frame = 1;
    shown.local_coverage_upper = 1.0;
    shown.visible_coverage = *visible;
    shown.present_reachable = true;
    shown.confidence = 1.0;
    if (!model.observe(shown)) throw std::runtime_error("visible model observation failed");

    arc::VisibilityObservation blocked = shown;
    blocked.id = 2;
    blocked.visible_coverage = *hidden;
    if (!model.observe(blocked)) throw std::runtime_error("hidden model observation failed");

    const auto* shown_state = model.find(1);
    const auto* hidden_state = model.find(2);
    if (!shown_state || shown_state->phase == arc::VisibilityPhase::Hidden)
        throw std::runtime_error("visible state wrong");
    if (!hidden_state || hidden_state->phase != arc::VisibilityPhase::Hidden)
        throw std::runtime_error("occluded state wrong");

    std::cout
        << "Stage 18 native D3D12 occlusion PASS: visible samples="
        << samples[0]
        << " hidden samples="
        << samples[1]
        << " visible coverage="
        << *visible
        << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}

#else
int main() { return 77; }
#endif
