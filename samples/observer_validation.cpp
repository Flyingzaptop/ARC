#include "arc/dx12_observer.hpp"
#include "arc/session.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <psapi.h>
#include "arc/statistics.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("D3D12 HRESULT " + std::to_string(static_cast<unsigned>(hr))); } }
static D3D12_RESOURCE_DESC buffer(UINT64 size) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
static std::uint64_t cpu_ticks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { throw std::runtime_error("GetProcessTimes failed"); }
    auto value = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return value(kernel) + value(user);
}
struct Owned { arc::ResourceId id{}; ComPtr<ID3D12Resource> resource; UINT64 bytes{}; };
int main(int argc, char** argv) try {
    const bool baseline = argc > 1 && std::string(argv[1]) == "baseline";
    const bool full = argc > 1 && std::string(argv[1]) == "full";
    const unsigned iterations = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 200;
    const double pressureFraction = argc > 3 ? std::stod(argv[3]) : 0.0;
    if (!(pressureFraction >= 0.0 && pressureFraction <= 0.85)) { throw std::runtime_error("pressure fraction must be 0..0.85"); }
    if (iterations == 0 || iterations > 100000) { throw std::runtime_error("iterations must be 1..100000"); }
    std::filesystem::create_directories("traces");
    const std::string stem = std::string(ARC_SAMPLE_NAME) + (baseline ? "-baseline" : full ? "-full" : "-light");
    std::unique_ptr<arc::Session> session;
    if (!baseline) { session = std::make_unique<arc::Session>("traces/" + stem + ".arcbin", 65536); }
    arc::EventRing inactiveRing(1);
    arc::IdAllocator ids;
    arc::dx12::Observer observer(session ? session->ring() : inactiveRing, ids);
    auto emit = [&](arc::EventType type, const auto& p) { if (!baseline && !observer.observe(type, p)) { throw std::runtime_error("trace overflow"); } };
    if (std::getenv("ARC_D3D12_DEBUG")) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    }
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter3> adapter; check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    auto budget = [&] { if (auto b = arc::dx12::query_memory_budget(adapter.Get())) { emit(arc::EventType::MemoryBudgetSample, *b); } };
    budget();
    std::vector<Owned> owned;
    auto allocate = [&](D3D12_RESOURCE_DESC d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) -> std::size_t {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        Owned o; check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&o.resource)));
        o.bytes = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
        o.id = baseline ? ids.next() : observer.observe_committed_resource(device.Get(), d, o.resource.Get());
        if (!o.id) { throw std::runtime_error("resource observation failed"); }
        owned.push_back(std::move(o)); return owned.size() - 1;
    };
    constexpr UINT64 bytes = 1024 * 1024;
    const auto upload = allocate(buffer(bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto gpu = allocate(buffer(bytes), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto readback = allocate(buffer(bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (pressureFraction > 0) {
        constexpr UINT64 increment = 32 * 1024 * 1024;
        for (unsigned step = 0; step < 256; ++step) {
            auto b = arc::dx12::query_memory_budget(adapter.Get());
            if (!b) { throw std::runtime_error("pressure test needs a dynamic budget"); }
            emit(arc::EventType::MemoryBudgetSample, *b);
            if (b->local_usage + increment >= static_cast<UINT64>(b->local_budget * pressureFraction)) { break; }
            allocate(buffer(increment), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    // Separate heap ownership from resource footprints, including aliasing.
    D3D12_HEAP_DESC hd{}; hd.SizeInBytes = 4 * bytes; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap; check(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    const auto heapId = baseline ? ids.next() : observer.observe_heap(hd, heap.Get());
    for (auto offset : {0ULL, 2 * bytes}) {
        Owned o; auto d = buffer(bytes); check(device->CreatePlacedResource(heap.Get(), offset, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&o.resource)));
        o.id = baseline ? ids.next() : observer.observe_placed_resource(device.Get(), heapId, offset, d, o.resource.Get());
        o.bytes = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes; owned.push_back(std::move(o));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{}; check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    const bool reservedSupported = options.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
    if (reservedSupported) {
        Owned o; auto d = buffer(2 * bytes);
        check(device->CreateReservedResource(&d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&o.resource)));
        o.id = baseline ? ids.next() : observer.observe_reserved_resource(d, o.resource.Get()); owned.push_back(std::move(o));
    }
    void* mapped{}; D3D12_RANGE empty{};
    check(owned[upload].resource->Map(0, &empty, &mapped));
    for (std::size_t i = 0; i < bytes; ++i) { static_cast<unsigned char*>(mapped)[i] = static_cast<unsigned char>((i * 17 + 31) & 255); }
    owned[upload].resource->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap, dsvHeap;
    for (auto entry : {std::pair{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, std::addressof(srvHeap)}, {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, std::addressof(rtvHeap)}, {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, std::addressof(dsvHeap)}}) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = entry.first; desc.NumDescriptors = 64;
        check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(entry.second->GetAddressOf())));
    }
    unsigned viewIndex{};
    std::vector<arc::DescriptorWrittenPayload> expectedViews;
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{}; cbv.BufferLocation = owned[upload].resource->GetGPUVirtualAddress(); cbv.SizeInBytes = 256;
        auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += viewIndex++ * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateConstantBufferView(&cbv, handle);
        const auto id = ids.next(); expectedViews.push_back({.descriptor = id, .resource = owned[upload].id, .type = arc::ViewType::Cbv, .buffer_bytes = 256});
        if (!baseline && !observer.observe_cbv(id, owned[upload].id, 0, 256)) { throw std::runtime_error("CBV observation failed"); }
    }
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{}; samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; samplerHeapDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap)));
    D3D12_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, samplerHeap->GetCPUDescriptorHandleForHeapStart());
    const auto samplerId = ids.next(); expectedViews.push_back({.descriptor = samplerId, .type = arc::ViewType::Sampler});
    if (!baseline && !observer.observe_sampler(samplerId)) { throw std::runtime_error("Sampler observation failed"); }
    if (std::string(ARC_SAMPLE_NAME) != "dx12-memory-pressure") {
        for (auto size : {512U, 1024U, 2048U, 4096U}) {
            for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC7_UNORM}) {
                D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = size; d.Height = size;
                d.DepthOrArraySize = 1; d.MipLevels = 5; d.Format = format; d.SampleDesc.Count = 1;
                auto index = allocate(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
                D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = format; v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 5;
                auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += viewIndex++ * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                device->CreateShaderResourceView(owned[index].resource.Get(), &v, handle);
                const auto descriptorId = ids.next();
                expectedViews.push_back({.descriptor = descriptorId, .resource = owned[index].id, .type = arc::ViewType::Srv, .mip_count = 5, .layer_count = 1, .format = static_cast<unsigned>(format)});
                if (!baseline && !observer.observe_srv(descriptorId, owned[index].id, v)) { throw std::runtime_error("SRV observation failed"); }
            }
        }
    }
    if (std::string(ARC_SAMPLE_NAME) == "dx12-mixed-resources") {
        for (auto type : {arc::ViewType::Uav, arc::ViewType::Rtv, arc::ViewType::Dsv}) {
            D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = d.Height = 512;
            d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
            d.Format = type == arc::ViewType::Dsv ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.Flags = type == arc::ViewType::Dsv ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : type == arc::ViewType::Rtv ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            auto index = allocate(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            if (type == arc::ViewType::Rtv) { device->CreateRenderTargetView(owned[index].resource.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart()); }
            else if (type == arc::ViewType::Dsv) { device->CreateDepthStencilView(owned[index].resource.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart()); }
            else { auto h = srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += viewIndex++ * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); device->CreateUnorderedAccessView(owned[index].resource.Get(), nullptr, nullptr, h); }
            emit(arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = ids.next(), .resource = owned[index].id, .type = type, .mip_count = 1, .layer_count = 1});
        }
    }
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, qd.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    const auto queueId = ids.next(), commandId = ids.next(), fenceId = ids.next();
    ComPtr<ID3D12CommandQueue> copyQueue; D3D12_COMMAND_QUEUE_DESC copyDesc{}; copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    check(device->CreateCommandQueue(&copyDesc, IID_PPV_ARGS(&copyQueue)));
    ComPtr<ID3D12CommandAllocator> copyAllocator; check(device->CreateCommandAllocator(copyDesc.Type, IID_PPV_ARGS(&copyAllocator)));
    ComPtr<ID3D12GraphicsCommandList> copyList; check(device->CreateCommandList(0, copyDesc.Type, copyAllocator.Get(), nullptr, IID_PPV_ARGS(&copyList)));
    ComPtr<ID3D12Fence> copyFence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence)));
    const auto copyQueueId = ids.next(), copyCommandId = ids.next(), copyFenceId = ids.next();
    emit(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = copyQueueId, .type = arc::QueueClass::Copy});
    emit(arc::EventType::CommandListCreated, arc::CommandListPayload{.command = copyCommandId, .type = arc::QueueClass::Copy});
    copyList->CopyBufferRegion(owned[readback].resource.Get(), 0, owned[gpu].resource.Get(), 0, bytes);
    emit(arc::EventType::CopyBuffer, arc::CopyPayload{.source = owned[gpu].id, .destination = owned[readback].id, .command = copyCommandId, .approximate_bytes = bytes});
    check(copyList->Close()); emit(arc::EventType::CommandListClosed, arc::CommandListPayload{.command = copyCommandId, .type = arc::QueueClass::Copy});
    // Hidden window: exercise DXGI without taking focus from the user.
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{}; windowClass.lpfnWndProc = DefWindowProcW; windowClass.hInstance = module; windowClass.lpszClassName = L"ARCStage1Validation";
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { throw std::runtime_error("RegisterClass failed"); }
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"ARC validation", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, module, nullptr);
    if (!window) { throw std::runtime_error("CreateWindow failed"); }
    DXGI_SWAP_CHAIN_DESC1 swapDesc{}; swapDesc.Width = swapDesc.Height = 64; swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1; swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; swapDesc.BufferCount = 2; swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap1; check(factory->CreateSwapChainForHwnd(queue.Get(), window, &swapDesc, nullptr, nullptr, &swap1));
    ComPtr<IDXGISwapChain3> swap; check(swap1.As(&swap));
    const auto swapId = ids.next(); emit(arc::EventType::SwapchainCreated, arc::PresentPayload{.swapchain = swapId});
    std::array<std::size_t, 2> backbuffers{};
    for (unsigned i = 0; i < 2; ++i) {
        Owned o; check(swap->GetBuffer(i, IID_PPV_ARGS(&o.resource))); o.id = ids.next();
        emit(arc::EventType::ResourceCreated, arc::ResourceCreatePayload{.resource = o.id, .virtual_bytes = 16384, .width = 64, .height = 64, .depth = 1, .mip_levels = 1, .array_layers = 1, .kind = arc::ResourceKind::Texture2D, .allocation_kind = arc::ResourceAllocationKind::External, .format = DXGI_FORMAT_R8G8B8A8_UNORM});
        backbuffers[i] = owned.size(); owned.push_back(std::move(o));
    }
    // A deterministic fullscreen draw provides an independently checkable image.
    D3D12_RESOURCE_DESC imageDesc{}; imageDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    imageDesc.Width = imageDesc.Height = 64; imageDesc.DepthOrArraySize = imageDesc.MipLevels = 1;
    imageDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; imageDesc.SampleDesc.Count = 1; imageDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto imageIndex = allocate(imageDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto imageReadback = allocate(buffer(64 * 256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto imageRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); imageRtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(owned[imageIndex].resource.Get(), nullptr, imageRtv);
    emit(arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = ids.next(), .resource = owned[imageIndex].id, .type = arc::ViewType::Rtv, .mip_count = 1, .layer_count = 1});
    D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rootBlob, errorBlob; check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob));
    ComPtr<ID3D12RootSignature> root; check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char* shader = "float4 vs(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); } float4 ps():SV_Target { return float4(1,0,1,1); }";
    ComPtr<ID3DBlob> vs, ps;
    check(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errorBlob));
    check(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errorBlob));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = root.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.SampleMask = UINT_MAX; pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pipeline.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = pipeline.BlendState.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.FrontFace = pipeline.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = imageDesc.Format; pipeline.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso; check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
    auto computeDesc = buffer(65536); computeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto computeBuffer = allocate(computeDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto computeReadback = allocate(buffer(65536), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_ROOT_PARAMETER computeParam{}; computeParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC computeRootDesc{}; computeRootDesc.NumParameters = 1; computeRootDesc.pParameters = &computeParam;
    check(D3D12SerializeRootSignature(&computeRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob));
    ComPtr<ID3D12RootSignature> computeRoot; check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&computeRoot)));
    const char* computeShader = "RWByteAddressBuffer result:register(u0); [numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID) { result.Store(id.x*4,id.x*13+7); }";
    ComPtr<ID3DBlob> cs; check(D3DCompile(computeShader, std::strlen(computeShader), nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &errorBlob));
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePipeline{}; computePipeline.pRootSignature = computeRoot.Get(); computePipeline.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> computePso; check(device->CreateComputePipelineState(&computePipeline, IID_PPV_ARGS(&computePso)));
    emit(arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = ids.next(), .resource = owned[computeBuffer].id, .type = arc::ViewType::Uav});
    const auto indexBuffer = allocate(buffer(65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    check(owned[indexBuffer].resource->Map(0, &empty, &mapped));
    auto indices = static_cast<unsigned*>(mapped); indices[0] = 0; indices[1] = 1; indices[2] = 2; owned[indexBuffer].resource->Unmap(0, nullptr);
    const auto indirectBuffer = allocate(buffer(65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_QUERY_HEAP_DESC queryDesc{}; queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queryDesc.Count = 8;
    ComPtr<ID3D12QueryHeap> queries; check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)));
    const auto timingReadback = allocate(buffer(65536), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFrequency{}; check(queue->GetTimestampFrequency(&timestampFrequency));
    if (!timestampFrequency) { throw std::runtime_error("invalid GPU timestamp frequency"); }
    std::array<std::vector<double>, 4> gpuTimes;
    check(owned[indirectBuffer].resource->Map(0, &empty, &mapped));
    const D3D12_DRAW_ARGUMENTS arguments{3, 1, 0, 0}; std::memcpy(mapped, &arguments, sizeof(arguments)); owned[indirectBuffer].resource->Unmap(0, nullptr);
    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{}; argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{}; signatureDesc.ByteStride = sizeof(arguments); signatureDesc.NumArgumentDescs = 1; signatureDesc.pArgumentDescs = &argumentDesc;
    ComPtr<ID3D12CommandSignature> signature; check(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)));
    emit(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = queueId, .type = arc::QueueClass::Graphics});
    emit(arc::EventType::CommandListCreated, arc::CommandListPayload{.command = commandId});
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!done) { throw std::runtime_error("CreateEvent failed"); }
    std::vector<double> times; times.reserve(iterations);
    const auto cpuStart = cpu_ticks(); const auto runStart = std::chrono::steady_clock::now();
    unsigned occludedPresents{};
    auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = owned[gpu].resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[gpu].id, .command = commandId, .before_state = static_cast<unsigned>(before), .after_state = static_cast<unsigned>(after), .subresource = UINT32_MAX});
    };
    for (unsigned frame = 0; frame < iterations; ++frame) {
        auto start = std::chrono::steady_clock::now();
        if (frame) { check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr)); emit(arc::EventType::CommandListReset, arc::CommandListPayload{.command = commandId}); transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST); }
        D3D12_RESOURCE_BARRIER imageBarrier{}; imageBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        imageBarrier.Transition.pResource = owned[imageIndex].resource.Get(); imageBarrier.Transition.Subresource = UINT_MAX;
        imageBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; imageBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (frame) {
            list->ResourceBarrier(1, &imageBarrier);
            emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[imageIndex].id, .command = commandId, .before_state = D3D12_RESOURCE_STATE_COPY_SOURCE, .after_state = D3D12_RESOURCE_STATE_RENDER_TARGET, .subresource = UINT_MAX});
        }
        list->SetGraphicsRootSignature(root.Get()); list->SetPipelineState(pso.Get());
        D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1}; D3D12_RECT scissor{0, 0, 64, 64};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor); list->OMSetRenderTargets(1, &imageRtv, FALSE, nullptr);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
        D3D12_INDEX_BUFFER_VIEW ib{owned[indexBuffer].resource->GetGPUVirtualAddress(), 12, DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&ib); list->DrawIndexedInstanced(3, 1, 0, 0, 0);
        list->ExecuteIndirect(signature.Get(), 1, owned[indirectBuffer].resource.Get(), 0, nullptr, 0);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = owned[indexBuffer].id});
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = owned[indirectBuffer].id});
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = owned[imageIndex].id, .write = 1});
        if (full) {
            emit(arc::EventType::Draw, arc::CountersPayload{.command = commandId, .draws = 1});
            emit(arc::EventType::DrawIndexed, arc::CountersPayload{.command = commandId, .indexed_draws = 1});
            emit(arc::EventType::ExecuteIndirect, arc::CountersPayload{.command = commandId, .indirect = 1});
        }
        emit(arc::EventType::CommandCounters, arc::CountersPayload{.command = commandId, .draws = 1, .indexed_draws = 1, .dispatches = 1, .indirect = 1});
        imageBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; imageBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &imageBarrier);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[imageIndex].id, .command = commandId, .before_state = D3D12_RESOURCE_STATE_RENDER_TARGET, .after_state = D3D12_RESOURCE_STATE_COPY_SOURCE, .subresource = UINT_MAX});
        D3D12_TEXTURE_COPY_LOCATION imageSource{}; imageSource.pResource = owned[imageIndex].resource.Get(); imageSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION imageDestination{}; imageDestination.pResource = owned[imageReadback].resource.Get(); imageDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        imageDestination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
        list->CopyTextureRegion(&imageDestination, 0, 0, 0, &imageSource, nullptr);
        emit(arc::EventType::CopyTexture, arc::CopyPayload{.source = owned[imageIndex].id, .destination = owned[imageReadback].id, .command = commandId, .approximate_bytes = 16384});
        D3D12_RESOURCE_BARRIER cb{}; cb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; cb.Transition.pResource = owned[computeBuffer].resource.Get(); cb.Transition.Subresource = UINT_MAX;
        cb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; cb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (frame) { list->ResourceBarrier(1, &cb); }
        if (frame) { emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[computeBuffer].id, .command = commandId, .before_state = D3D12_RESOURCE_STATE_COPY_SOURCE, .after_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, .subresource = UINT_MAX}); }
        list->SetComputeRootSignature(computeRoot.Get()); list->SetPipelineState(computePso.Get());
        list->SetComputeRootUnorderedAccessView(0, owned[computeBuffer].resource->GetGPUVirtualAddress());
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2); list->Dispatch(256, 1, 1);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = owned[computeBuffer].id, .write = 1});
        if (full) { emit(arc::EventType::Dispatch, arc::CountersPayload{.command = commandId, .dispatches = 1}); }
        cb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; cb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; list->ResourceBarrier(1, &cb);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[computeBuffer].id, .command = commandId, .before_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, .after_state = D3D12_RESOURCE_STATE_COPY_SOURCE, .subresource = UINT_MAX});
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
        list->CopyResource(owned[computeReadback].resource.Get(), owned[computeBuffer].resource.Get());
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
        emit(arc::EventType::CopyResource, arc::CopyPayload{.source = owned[computeBuffer].id, .destination = owned[computeReadback].id, .command = commandId, .approximate_bytes = 65536});
        const auto backbuffer = backbuffers[swap->GetCurrentBackBufferIndex()];
        D3D12_RESOURCE_BARRIER bb{}; bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bb.Transition.pResource = owned[backbuffer].resource.Get();
        bb.Transition.Subresource = UINT_MAX; bb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; bb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &bb);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[backbuffer].id, .command = commandId, .after_state = D3D12_RESOURCE_STATE_COPY_DEST, .subresource = UINT_MAX});
        list->CopyResource(owned[backbuffer].resource.Get(), owned[imageIndex].resource.Get());
        emit(arc::EventType::CopyResource, arc::CopyPayload{.source = owned[imageIndex].id, .destination = owned[backbuffer].id, .command = commandId, .approximate_bytes = 16384});
        bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; bb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &bb);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = owned[backbuffer].id, .command = commandId, .before_state = D3D12_RESOURCE_STATE_COPY_DEST, .subresource = UINT_MAX});
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
        list->CopyBufferRegion(owned[gpu].resource.Get(), 0, owned[upload].resource.Get(), 0, bytes);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
        emit(arc::EventType::CopyBuffer, arc::CopyPayload{.source = owned[upload].id, .destination = owned[gpu].id, .command = commandId, .approximate_bytes = bytes});
        transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (full) { emit(arc::EventType::TelemetrySample, arc::CountersPayload{.command = commandId}); }
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, owned[timingReadback].resource.Get(), 0);
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = owned[timingReadback].id, .write = 1});
        check(list->Close()); emit(arc::EventType::CommandListClosed, arc::CommandListPayload{.command = commandId});
        ID3D12CommandList* submit[] = {list.Get()}; queue->ExecuteCommandLists(1, submit);
        emit(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = queueId, .command = commandId, .submission = frame + 1ULL});
        check(queue->Signal(fence.Get(), frame + 1ULL)); emit(arc::EventType::FenceSignal, arc::FencePayload{.queue = queueId, .fence = fenceId, .value = frame + 1ULL});
        check(copyQueue->Wait(fence.Get(), frame + 1ULL));
        emit(arc::EventType::FenceWait, arc::FencePayload{.queue = copyQueueId, .fence = fenceId, .value = frame + 1ULL});
        ID3D12CommandList* copySubmit[] = {copyList.Get()}; copyQueue->ExecuteCommandLists(1, copySubmit);
        emit(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = copyQueueId, .command = copyCommandId, .submission = frame + 1ULL});
        check(copyQueue->Signal(copyFence.Get(), frame + 1ULL));
        emit(arc::EventType::FenceSignal, arc::FencePayload{.queue = copyQueueId, .fence = copyFenceId, .value = frame + 1ULL});
        check(copyFence->SetEventOnCompletion(frame + 1ULL, done));
        if (WaitForSingleObject(done, 10000) != WAIT_OBJECT_0) { throw std::runtime_error("GPU fence timeout"); }
        // CPU wait uses queue=0, never implies a GPU queue dependency.
        emit(arc::EventType::FenceWait, arc::FencePayload{.fence = copyFenceId, .value = frame + 1ULL});
        UINT64* timestamps{}; D3D12_RANGE timingRange{0, 8 * sizeof(UINT64)};
        check(owned[timingReadback].resource->Map(0, &timingRange, reinterpret_cast<void**>(&timestamps)));
        for (unsigned i = 0; i < 4; ++i) {
            if (timestamps[2 * i + 1] < timestamps[2 * i]) { throw std::runtime_error("GPU timestamp order error"); }
            if (frame >= 10 || iterations <= 10) { gpuTimes[i].push_back(1000.0 * static_cast<double>(timestamps[2 * i + 1] - timestamps[2 * i]) / timestampFrequency); }
        }
        owned[timingReadback].resource->Unmap(0, &empty);
        const auto presentResult = swap->Present(0, 0); check(presentResult);
        occludedPresents += presentResult == DXGI_STATUS_OCCLUDED;
        emit(arc::EventType::Present, arc::PresentPayload{.swapchain = swapId, .frame = frame + 1ULL, .result = presentResult});
        if (frame % 60 == 0) { budget(); }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const auto cpuMs = static_cast<double>(cpu_ticks() - cpuStart) / 10000.0;
    const auto wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) { throw std::runtime_error("GetProcessMemoryInfo failed"); }
    CloseHandle(done);
    D3D12_RANGE range{0, bytes}; check(owned[readback].resource->Map(0, &range, &mapped));
    bool contents = true;
    for (std::size_t i = 0; i < bytes; ++i) { if (static_cast<unsigned char*>(mapped)[i] != static_cast<unsigned char>((i * 17 + 31) & 255)) { contents = false; break; } }
    owned[readback].resource->Unmap(0, &empty);
    D3D12_RANGE imageRange{0, 16384}; check(owned[imageReadback].resource->Map(0, &imageRange, &mapped));
    const unsigned char color[] = {255, 0, 255, 255};
    for (unsigned i = 0; i < 16384; ++i) { if (static_cast<unsigned char*>(mapped)[i] != color[i % 4]) { contents = false; break; } }
    owned[imageReadback].resource->Unmap(0, &empty);
    D3D12_RANGE computeRange{0, 65536}; check(owned[computeReadback].resource->Map(0, &computeRange, &mapped));
    for (unsigned i = 0; i < 16384; ++i) { if (static_cast<unsigned*>(mapped)[i] != i * 13 + 7) { contents = false; break; } }
    owned[computeReadback].resource->Unmap(0, &empty);
    UINT64 expectedBytes{}; for (auto& o : owned) { expectedBytes += o.bytes; o.resource.Reset(); if (!baseline) { observer.observe_resource_destroyed(o.id); } }
    heap.Reset(); if (!baseline) { observer.observe_heap_destroyed(heapId); }
    swap.Reset(); swap1.Reset(); DestroyWindow(window);
    if (session) { session->finish(); }
    bool valid = contents && (!session || session->complete());
    if (std::getenv("ARC_D3D12_DEBUG")) {
        ComPtr<ID3D12InfoQueue> info; check(device.As(&info));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T length{}; check(info->GetMessage(i, nullptr, &length)); std::vector<std::byte> storage(length);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); check(info->GetMessage(i, message, &length));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; valid = false; }
        }
    }
    std::size_t matchedCreates{}, matchedDestroys{}, matchedViews{};
    std::uint64_t observedBytes{};
    if (!baseline) {
        const auto& g = session->graph();
        valid = valid && g.resource_count() == owned.size() && g.live_allocation_bytes() == 0 && g.errors() == 0 && g.submissions().size() == 2ULL * iterations && g.copies().size() == 5ULL * iterations;
        for (const auto& sub : g.submissions()) {
            if (sub.description.queue == queueId) { valid = valid && sub.counters.draws == 1 && sub.counters.indexed_draws == 1 && sub.counters.dispatches == 1 && sub.counters.indirect == 1; }
        }
        for (const auto& o : owned) {
            auto r = g.find(o.id);
            matchedCreates += r.has_value(); matchedDestroys += r && !r->alive;
            if (r) { observedBytes += r->description.allocation_bytes; }
            valid = valid && r && !r->alive && r->description.allocation_bytes == o.bytes;
        }
        valid = valid && g.live_heap_bytes() == 0 && g.resources_on_heap(heapId).size() == 2;
        for (const auto& expected : expectedViews) {
            const auto actual = g.find_view(expected.descriptor);
            matchedViews += actual && actual->description.resource == expected.resource && actual->description.type == expected.type && actual->description.mip_count == expected.mip_count && actual->description.layer_count == expected.layer_count && actual->description.buffer_bytes == expected.buffer_bytes;
        }
        valid = valid && matchedViews == expectedViews.size();
        valid = valid && arc::TraceReader::inspect("traces/" + stem + ".arcbin").status == arc::TraceReader::Status::Complete;
    }
    std::sort(times.begin(), times.end());
    const auto stats = arc::summarize(times);
    auto percentile = [&](double p) { return times[static_cast<std::size_t>(p * (times.size() - 1))]; };
    std::ofstream report("traces/" + stem + ".json");
    report << "{\n\"sample\":\"" << ARC_SAMPLE_NAME << "\",\"iterations\":" << iterations << ",\"resources\":" << owned.size()
        << ",\"expected_allocation_bytes\":" << expectedBytes << ",\"contents_match\":" << (contents ? "true" : "false")
        << ",\"valid\":" << (valid ? "true" : "false") << ",\"dropped\":" << (session ? session->dropped() : 0)
        << ",\"iteration_ms_p50\":" << percentile(.5) << ",\"iteration_ms_p95\":" << percentile(.95) << ",\"iteration_ms_p99\":" << percentile(.99) << "}\n";
    report.close();
    // Detailed measured sidecar; values describe this controlled workload only.
    std::ofstream metrics("traces/" + stem + "-metrics.json");
    metrics << "{\"schema\":1,\"cpu_ms\":" << cpuMs << ",\"wall_ms\":" << wallMs
        << ",\"cpu_one_core_percent\":" << 100 * cpuMs / wallMs << ",\"process_private_bytes\":" << memory.PrivateUsage
        << ",\"trace_bytes\":" << (baseline ? 0 : std::filesystem::file_size("traces/" + stem + ".arcbin"))
        << ",\"mean_iteration_ms\":" << stats.mean << ",\"iteration_variance\":" << stats.variance
        << ",\"iterations_per_second\":" << 1000.0 / stats.mean << ",\"occluded_presents\":" << occludedPresents
        << ",\"reserved_supported\":" << (reservedSupported ? "true" : "false")
        << ",\"create_recall\":" << (baseline ? "null" : std::to_string(static_cast<double>(matchedCreates) / owned.size()))
        << ",\"destroy_recall\":" << (baseline ? "null" : std::to_string(static_cast<double>(matchedDestroys) / owned.size()))
        << ",\"descriptor_mapping_accuracy\":" << (baseline ? "null" : std::to_string(static_cast<double>(matchedViews) / expectedViews.size()))
        << ",\"allocation_error_bytes\":" << (baseline ? "null" : std::to_string(observedBytes > expectedBytes ? observedBytes - expectedBytes : expectedBytes - observedBytes)) << ",\"gpu_proxies_ms\":{";
    const char* gpuNames[] = {"three_draws_64x64", "compute_16384_uints", "upload_1MiB", "readback_64KiB"};
    for (unsigned i = 0; i < 4; ++i) {
        if (i) { metrics << ','; } const auto s = arc::summarize(gpuTimes[i]);
        metrics << '"' << gpuNames[i] << "\":{\"median\":" << s.median << ",\"p10\":" << s.p10 << ",\"p90\":" << s.p90 << ",\"variance\":" << s.variance << '}';
    }
    metrics << "}}\n";
    std::cout << stem << ": valid=" << valid << " resources=" << owned.size() << " iterations=" << iterations << " p50_ms=" << percentile(.5) << " p99_ms=" << percentile(.99) << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
