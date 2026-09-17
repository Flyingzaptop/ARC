#include "arc/dx12_runtime_backend.hpp"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(what);
}

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
}

int main() try {
    const bool debug_requested = std::getenv("ARC_D3D12_DEBUG") != nullptr;
    if (debug_requested) {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return 77;
        debug->EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 77;

    std::atomic<unsigned> debug_errors{};
    ComPtr<ID3D12InfoQueue1> info_queue;
    DWORD callback_cookie{};
    if (debug_requested) {
        check(device.As(&info_queue), "ID3D12InfoQueue1 unavailable");
        check(info_queue->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR, void* context) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    static_cast<std::atomic<unsigned>*>(context)->fetch_add(1, std::memory_order_relaxed);
                }
            },
            D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS,
            &debug_errors,
            &callback_cookie),
            "RegisterMessageCallback");
    }
    struct DebugGuard {
        ID3D12InfoQueue1* queue{};
        DWORD cookie{};
        ~DebugGuard() { if (queue) queue->UnregisterMessageCallback(cookie); }
    } debug_guard{info_queue.Get(), callback_cookie};

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto desc = buffer_desc(8ull * 1024ull * 1024ull);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&resource)),
        "CreateCommittedResource");

    constexpr arc::ResourceId resource_id = 101;
    constexpr arc::QueueId queue_id = 7;
    arc::dx12::LiveRuntimeBackend backend(device.Get());
    if (!backend.bind_resource(resource_id, resource.Get()) || !backend.bind_queue_fence(queue_id, fence.Get())) {
        throw std::runtime_error("backend binding failed");
    }

    check(queue->Signal(fence.Get(), 1), "Queue Signal");
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) throw std::runtime_error("CreateEvent failed");
    struct HandleGuard { HANDLE value{}; ~HandleGuard(){ if (value) CloseHandle(value); } } ready_guard{ready};
    check(fence->SetEventOnCompletion(1, ready), "SetEventOnCompletion");
    if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0) throw std::runtime_error("fence timeout");

    arc::ResidencyAction evict{
        arc::ResidencyAction::Type::Evict,
        1,
        desc.Width,
        0,
        1.0,
        queue_id,
        1};
    const auto evict_status = backend.evict(resource_id, evict);

    arc::ResidencyAction resident = evict;
    resident.type = arc::ResidencyAction::Type::MakeResident;
    resident.required_queue = 0;
    resident.required_fence = 0;
    const auto resident_status = backend.make_resident(resource_id, resident);
    const auto missing_status = backend.evict(9999, evict);

    arc::TextureQualityAction demote{
        arc::TextureQualityAction::Type::Demote,
        10,
        202,
        0,
        1,
        4ull * 1024ull * 1024ull,
        -0.05,
        1.0};
    const auto no_callback_status = backend.demote_texture(202, demote);

    unsigned texture_calls{};
    backend.set_texture_mutator([&](const arc::TextureQualityAction& action) {
        ++texture_calls;
        return action.resource == 202
            ? arc::RuntimeBackendStatus::Success
            : arc::RuntimeBackendStatus::Failure;
    });
    const auto demote_status = backend.demote_texture(202, demote);
    auto promote = demote;
    promote.type = arc::TextureQualityAction::Type::Promote;
    promote.from_level = 1;
    promote.to_level = 0;
    promote.quality_delta = 0.05;
    const auto promote_status = backend.promote_texture(202, promote);

    const bool unbound_resource = backend.unbind_resource(resource_id) && !backend.has_resource(resource_id);
    const bool unbound_fence = backend.unbind_queue_fence(queue_id) && !backend.has_queue_fence(queue_id);
    const bool valid =
        evict_status == arc::RuntimeBackendStatus::Success &&
        resident_status == arc::RuntimeBackendStatus::Success &&
        missing_status == arc::RuntimeBackendStatus::Unsupported &&
        no_callback_status == arc::RuntimeBackendStatus::Unsupported &&
        demote_status == arc::RuntimeBackendStatus::Success &&
        promote_status == arc::RuntimeBackendStatus::Success &&
        texture_calls == 2 && unbound_resource && unbound_fence &&
        debug_errors.load(std::memory_order_relaxed) == 0;

    std::filesystem::create_directories("traces");
    std::ofstream out("traces/runtime-backend-lab.json", std::ios::trunc);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
        << "  \"evict_ok\": " << (evict_status == arc::RuntimeBackendStatus::Success ? "true" : "false") << ",\n"
        << "  \"make_resident_ok\": " << (resident_status == arc::RuntimeBackendStatus::Success ? "true" : "false") << ",\n"
        << "  \"missing_binding_blocked\": " << (missing_status == arc::RuntimeBackendStatus::Unsupported ? "true" : "false") << ",\n"
        << "  \"texture_without_callback_blocked\": " << (no_callback_status == arc::RuntimeBackendStatus::Unsupported ? "true" : "false") << ",\n"
        << "  \"texture_callback_calls\": " << texture_calls << ",\n"
        << "  \"resource_unbound\": " << (unbound_resource ? "true" : "false") << ",\n"
        << "  \"queue_fence_unbound\": " << (unbound_fence ? "true" : "false") << ",\n"
        << "  \"debug_error_count\": " << debug_errors.load(std::memory_order_relaxed) << "\n"
        << "}\n";

    return valid ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "runtime-backend-lab: " << error.what() << '\n';
    return 1;
}
