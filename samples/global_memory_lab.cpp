#include "arc/dx12_observer.hpp"
#include "arc/dx12_residency.hpp"
#include "arc/memory_planner.hpp"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kBufferBytes = 64ull * kMiB;
constexpr UINT64 kTileBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr UINT kTextureWidth = 4096;
constexpr UINT kTextureHeight = 4096;
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(what) + " failed with HRESULT " + std::to_string(static_cast<unsigned>(hr)));
    }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

UINT mip_count(UINT width, UINT height) {
    UINT result = 1;
    while (width > 1 || height > 1) {
        width = (std::max)(1U, width >> 1U);
        height = (std::max)(1U, height >> 1U);
        ++result;
    }
    return result;
}

struct TileRegion {
    D3D12_TILED_RESOURCE_COORDINATE coordinate{};
    D3D12_TILE_REGION_SIZE size{};
    UINT tile_count{};
    ComPtr<ID3D12Heap> heap;
};

struct Transfer {
    UINT mip{};
    std::uint8_t pattern{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_bytes{};
    UINT64 total_bytes{};
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
};

ComPtr<ID3D12Heap> create_tile_heap(ID3D12Device* device, UINT tiles) {
    D3D12_HEAP_DESC d{};
    d.SizeInBytes = static_cast<UINT64>(tiles) * kTileBytes;
    d.Properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    d.Flags = static_cast<D3D12_HEAP_FLAGS>(D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES);
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&d, IID_PPV_ARGS(&heap)), "Create tile heap");
    return heap;
}

Transfer create_transfer(ID3D12Device* device, const D3D12_RESOURCE_DESC& texture_desc, UINT mip, std::uint8_t pattern) {
    Transfer t{};
    t.mip = mip;
    t.pattern = pattern;
    device->GetCopyableFootprints(&texture_desc, mip, 1, 0, &t.footprint, &t.rows, &t.row_bytes, &t.total_bytes);
    if (!t.total_bytes || !t.row_bytes || !t.rows) throw std::runtime_error("invalid texture footprint");
    const auto bd = buffer_desc(t.total_bytes);
    auto upload_props = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    auto readback_props = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&t.upload)), "Create texture upload");
    check(device->CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.readback)), "Create texture readback");
    void* mapped = nullptr;
    D3D12_RANGE empty{0, 0};
    check(t.upload->Map(0, &empty, &mapped), "Map texture upload");
    auto* bytes = static_cast<std::uint8_t*>(mapped) + t.footprint.Offset;
    for (UINT row = 0; row < t.rows; ++row) {
        std::memset(bytes + static_cast<std::size_t>(row) * t.footprint.Footprint.RowPitch, pattern, static_cast<std::size_t>(t.row_bytes));
    }
    t.upload->Unmap(0, nullptr);
    return t;
}

void record_texture_upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, const Transfer& t) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = t.mip;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = t.footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void record_texture_readback(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, const Transfer& t) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = t.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = t.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = t.mip;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

bool verify_texture(const Transfer& t) {
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(t.total_bytes)};
    if (FAILED(t.readback->Map(0, &range, &mapped))) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + t.footprint.Offset;
    bool ok = true;
    for (UINT row = 0; row < t.rows && ok; ++row) {
        const auto* start = bytes + static_cast<std::size_t>(row) * t.footprint.Footprint.RowPitch;
        for (UINT64 col = 0; col < t.row_bytes; ++col) {
            if (start[col] != t.pattern) { ok = false; break; }
        }
    }
    D3D12_RANGE empty{0, 0};
    t.readback->Unmap(0, &empty);
    return ok;
}

bool verify_buffer(ID3D12Resource* readback, std::uint8_t pattern) {
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(kBufferBytes)};
    if (FAILED(readback->Map(0, &range, &mapped))) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    bool ok = true;
    for (std::uint64_t i = 0; i < kBufferBytes; ++i) {
        if (bytes[i] != pattern) { ok = false; break; }
    }
    D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);
    return ok;
}

std::uint64_t debug_error_count(ID3D12InfoQueue* q) {
    if (!q) return 0;
    std::uint64_t errors = 0;
    const auto count = q->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(q->GetMessage(i, nullptr, &bytes)) || !bytes) continue;
        std::vector<std::uint8_t> storage(bytes);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(q->GetMessage(i, m, &bytes)) &&
            (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) ++errors;
    }
    return errors;
}

arc::TextureQualityAction demotion_action(const arc::TextureQualityGovernor& quality, const arc::MemoryActionCandidate& candidate) {
    const auto object = quality.find(candidate.subject);
    if (!object || object->current_level >= object->demotion_steps.size()) return {};
    const auto level = object->current_level;
    const auto& step = object->demotion_steps[level];
    return {arc::TextureQualityAction::Type::Demote, object->id, object->resource, level, level + 1, step.bytes_freed, -(step.quality_loss * object->importance), 0.0};
}

arc::TextureQualityAction promotion_action(const arc::TextureQualityGovernor& quality, const arc::MemoryRestoreCandidate& candidate) {
    const auto object = quality.find(candidate.subject);
    if (!object || !object->current_level) return {};
    const auto level = object->current_level;
    const auto& step = object->demotion_steps[level - 1];
    return {arc::TextureQualityAction::Type::Promote, object->id, object->resource, level, level - 1, step.bytes_freed, step.quality_loss * object->importance, 0.0};
}

} // namespace

int main() try {
    const bool debug_enabled = std::getenv("ARC_D3D12_DEBUG") != nullptr;
    if (debug_enabled) {
        ComPtr<ID3D12Debug> debug;
        const auto hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (FAILED(hr)) { std::cout << "SKIP: D3D12 debug layer unavailable\n"; return 77; }
        debug->EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    ComPtr<ID3D12Device3> device3;
    if (FAILED(device.As(&device3))) { std::cout << "SKIP: ID3D12Device3 unavailable\n"; return 77; }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "CheckFeatureSupport");
    if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) { std::cout << "SKIP: tiled resources unavailable\n"; return 77; }

    ComPtr<ID3D12InfoQueue> info_queue;
    if (debug_enabled) check(device.As(&info_queue), "ID3D12InfoQueue");

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter3> adapter;
    check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEventW failed");
    std::uint64_t fence_value = 0;
    auto wait_queue = [&] {
        check(queue->Signal(fence.Get(), ++fence_value), "Signal");
        check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    };

    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    auto execute = [&] {
        check(list->Close(), "Close command list");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        wait_queue();
    };
    auto reset = [&] {
        check(allocator->Reset(), "Reset allocator");
        check(list->Reset(allocator.Get(), nullptr), "Reset command list");
    };

    auto default_props = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    auto upload_props = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    auto readback_props = heap_properties(D3D12_HEAP_TYPE_READBACK);
    const auto bd = buffer_desc(kBufferBytes);
    ComPtr<ID3D12Resource> buffer, buffer_upload, buffer_readback;
    check(device->CreateCommittedResource(&default_props, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)), "Create controlled buffer");
    check(device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer_upload)), "Create buffer upload");
    check(device->CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer_readback)), "Create buffer readback");
    void* buffer_map = nullptr;
    D3D12_RANGE empty{0, 0};
    check(buffer_upload->Map(0, &empty, &buffer_map), "Map buffer upload");
    std::memset(buffer_map, 0x5A, static_cast<std::size_t>(kBufferBytes));
    buffer_upload->Unmap(0, nullptr);

    const UINT mip_levels = mip_count(kTextureWidth, kTextureHeight);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTextureWidth;
    td.Height = kTextureHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>(mip_levels);
    td.Format = kTextureFormat;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ComPtr<ID3D12Resource> texture;
    check(device->CreateReservedResource(&td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "CreateReservedResource");

    UINT total_tiles = 0;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE tile_shape{};
    UINT tiling_count = mip_levels;
    std::vector<D3D12_SUBRESOURCE_TILING> tilings(tiling_count);
    device->GetResourceTiling(texture.Get(), &total_tiles, &packed, &tile_shape, &tiling_count, 0, tilings.data());
    if (packed.NumStandardMips < 3 || !total_tiles) { CloseHandle(event); std::cout << "SKIP: insufficient standard mips\n"; return 77; }

    std::vector<TileRegion> regions;
    regions.reserve(packed.NumStandardMips + (packed.NumPackedMips ? 1U : 0U));
    UINT accounted_tiles = 0;
    for (UINT mip = 0; mip < packed.NumStandardMips; ++mip) {
        TileRegion r{};
        r.coordinate = {0, 0, 0, mip};
        r.size.UseBox = TRUE;
        r.size.Width = tilings[mip].WidthInTiles;
        r.size.Height = tilings[mip].HeightInTiles;
        r.size.Depth = tilings[mip].DepthInTiles;
        r.tile_count = r.size.Width * r.size.Height * r.size.Depth;
        r.size.NumTiles = r.tile_count;
        r.heap = create_tile_heap(device.Get(), r.tile_count);
        accounted_tiles += r.tile_count;
        regions.push_back(std::move(r));
    }
    if (packed.NumPackedMips) {
        TileRegion r{};
        r.coordinate = {0, 0, 0, packed.NumStandardMips};
        r.size.UseBox = FALSE;
        r.size.NumTiles = packed.NumTilesForPackedMips;
        r.tile_count = packed.NumTilesForPackedMips;
        r.heap = create_tile_heap(device.Get(), r.tile_count);
        accounted_tiles += r.tile_count;
        regions.push_back(std::move(r));
    }
    if (accounted_tiles != total_tiles) throw std::runtime_error("tile accounting mismatch");

    auto map_region = [&](UINT mip, bool map) {
        auto& r = regions[mip];
        const D3D12_TILE_RANGE_FLAGS flag = map ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
        const UINT heap_offset = 0;
        const UINT count = r.tile_count;
        queue->UpdateTileMappings(texture.Get(), 1, &r.coordinate, &r.size, map ? r.heap.Get() : nullptr, 1, &flag, &heap_offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    };
    for (UINT i = 0; i < regions.size(); ++i) map_region(i, true);
    wait_queue();

    std::array<Transfer, 3> transfers{
        create_transfer(device.Get(), td, 0, 0x11),
        create_transfer(device.Get(), td, 1, 0x22),
        create_transfer(device.Get(), td, 2, 0x44)};

    list->CopyBufferRegion(buffer.Get(), 0, buffer_upload.Get(), 0, kBufferBytes);
    D3D12_RESOURCE_BARRIER buffer_barrier{};
    buffer_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    buffer_barrier.Transition.pResource = buffer.Get();
    buffer_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    buffer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    buffer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &buffer_barrier);
    for (const auto& t : transfers) record_texture_upload(list.Get(), texture.Get(), t);
    D3D12_RESOURCE_BARRIER texture_barrier{};
    texture_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    texture_barrier.Transition.pResource = texture.Get();
    texture_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    texture_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    texture_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &texture_barrier);
    list->CopyBufferRegion(buffer_readback.Get(), 0, buffer.Get(), 0, kBufferBytes);
    for (const auto& t : transfers) record_texture_readback(list.Get(), texture.Get(), t);
    execute();

    const bool initial_buffer_ok = verify_buffer(buffer_readback.Get(), 0x5A);
    bool initial_texture_ok = true;
    for (const auto& t : transfers) initial_texture_ok = initial_texture_ok && verify_texture(t);
    if (!initial_buffer_ok || !initial_texture_ok) throw std::runtime_error("initial content verification failed");

    const auto before_relief = arc::dx12::query_memory_budget(adapter.Get());
    if (!before_relief) throw std::runtime_error("DXGI budget unavailable");

    arc::ResidencyPolicyConfig residency_cfg{};
    residency_cfg.minimum_residency_age_epochs = 1;
    residency_cfg.minimum_prefetch_confidence = 0.15;
    residency_cfg.prefetch_horizon_epochs = 8;
    residency_cfg.eviction_prediction_guard_epochs = 8;
    residency_cfg.recovery_samples = 1;
    arc::ResidencyGovernor residency(residency_cfg);
    arc::ResidencyObject ro{};
    ro.id = 1;
    ro.resource = 1001;
    ro.state = arc::ResidencyState::Resident;
    ro.safety = arc::ResidencySafety::ControlledSafe;
    ro.cost.bytes = kBufferBytes;
    ro.cost.reload_ms = 1.5;
    if (!residency.register_object(ro)) throw std::runtime_error("residency registration failed");
    for (std::uint64_t epoch : {1ull, 41ull, 81ull}) {
        if (!residency.note_use(1, epoch, fence_value, fence_value)) throw std::runtime_error("note_use failed");
    }

    arc::TextureQualityPolicyConfig quality_cfg{};
    quality_cfg.minimum_change_age_epochs = 0;
    arc::TextureQualityGovernor quality(quality_cfg);
    const auto total_texture_bytes = static_cast<std::uint64_t>(total_tiles) * kTileBytes;
    const auto mip0_bytes = static_cast<std::uint64_t>(regions[0].tile_count) * kTileBytes;
    const auto mip1_bytes = static_cast<std::uint64_t>(regions[1].tile_count) * kTileBytes;
    if (!quality.register_texture({.id=1,.resource=2001,.safety=arc::TextureQualitySafety::MipSafe,.full_resident_bytes=total_texture_bytes,.demotion_steps={{mip0_bytes,.01},{mip1_bytes,.04}},.current_level=0,.importance=1.0,.last_change_epoch=0})) {
        throw std::runtime_error("quality registration failed");
    }

    arc::GlobalMemoryPlannerConfig planner_cfg{};
    planner_cfg.arbiter.quality_weight = 1.0;
    planner_cfg.arbiter.latency_weight = 1.0;
    planner_cfg.arbiter.uncertainty_weight = 0.01;
    arc::GlobalMemoryPlanner planner(planner_cfg);

    const std::uint64_t synthetic_budget = 512ull * kMiB;
    const std::uint64_t synthetic_usage = 500ull * kMiB;
    residency.update_budget(synthetic_budget, synthetic_usage);
    const auto relief = planner.plan_pressure_relief(residency, quality, 100);
    if (relief.arbitration.shortfall || relief.arbitration.actions.empty()) throw std::runtime_error("global pressure plan shortfall");

    arc::dx12::ResidencyBackend residency_backend(device.Get());
    std::uint64_t expected_freed = 0;
    std::uint32_t eviction_actions = 0;
    std::uint32_t demotion_actions = 0;
    for (const auto& planned : relief.arbitration.actions) {
        const auto& c = planned.candidate;
        expected_freed += c.bytes_freed;
        if (c.kind == arc::MemoryActionKind::EvictResource) {
            ++eviction_actions;
            const auto result = residency_backend.evict_after(buffer.Get(), fence.Get(), fence_value);
            if (result != arc::dx12::ResidencyResult::Success) throw std::runtime_error("buffer eviction failed");
            if (!residency.transition(c.subject, arc::ResidencyState::Resident, arc::ResidencyState::Evicted)) throw std::runtime_error("residency state eviction failed");
            residency.record_eviction(c.subject, false, 100);
        } else {
            ++demotion_actions;
            const auto action = demotion_action(quality, c);
            if (!action.texture || action.from_level >= 2) throw std::runtime_error("unexpected texture demotion");
            const UINT mip = action.from_level;
            map_region(mip, false);
            wait_queue();
            regions[mip].heap.Reset();
            if (!quality.apply(action, 100)) throw std::runtime_error("quality demotion apply failed");
        }
    }
    if (!eviction_actions || !demotion_actions) throw std::runtime_error("global planner did not exercise both action classes");

    std::uint64_t minimum_usage = before_relief->local_usage;
    for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (const auto b = arc::dx12::query_memory_budget(adapter.Get())) minimum_usage = (std::min)(minimum_usage, b->local_usage);
    }
    const auto observed_drop = before_relief->local_usage > minimum_usage ? before_relief->local_usage - minimum_usage : 0;

    reset();
    record_texture_readback(list.Get(), texture.Get(), transfers[2]);
    execute();
    const bool lower_mip_preserved = verify_texture(transfers[2]);
    if (!lower_mip_preserved) throw std::runtime_error("lower mip changed during global relief");

    // Recovery from Emergency is deliberately hysteretic: one safe sample moves
    // Emergency -> Pressure, the next safe sample moves Pressure -> Normal.
    residency.update_budget(synthetic_budget, 300ull * kMiB);
    if (residency.pressure() != arc::PressureState::Pressure) throw std::runtime_error("expected Emergency -> Pressure recovery");
    residency.update_budget(synthetic_budget, 300ull * kMiB);
    if (residency.pressure() != arc::PressureState::Normal) throw std::runtime_error("expected Pressure -> Normal recovery");
    const auto restore = planner.plan_headroom_restore(residency, quality, 115, expected_freed);
    if (restore.arbitration.planned_bytes > expected_freed) throw std::runtime_error("restore exceeded headroom");

    std::uint32_t make_resident_actions = 0;
    std::uint32_t promotion_actions = 0;
    std::array<bool, 2> restored_mips{false, false};
    for (const auto& planned : restore.arbitration.actions) {
        const auto& c = planned.candidate;
        if (c.kind == arc::MemoryRestoreKind::MakeResident) {
            ++make_resident_actions;
            if (!residency.transition(c.subject, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) throw std::runtime_error("pending resident transition failed");
            if (residency_backend.make_resident_and_wait(buffer.Get()) != arc::dx12::ResidencyResult::Success) throw std::runtime_error("buffer make-resident failed");
            if (!residency.transition(c.subject, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident)) throw std::runtime_error("resident transition failed");
            residency.record_resident(c.subject, false, 115);
        } else {
            ++promotion_actions;
            const auto action = promotion_action(quality, c);
            if (!action.texture || action.to_level >= 2) throw std::runtime_error("unexpected texture promotion");
            const UINT mip = action.to_level;
            regions[mip].heap = create_tile_heap(device.Get(), regions[mip].tile_count);
            map_region(mip, true);
            wait_queue();
            restored_mips[mip] = true;
            if (!quality.apply(action, 115)) throw std::runtime_error("quality promotion apply failed");
        }
    }
    if (!make_resident_actions || !promotion_actions) throw std::runtime_error("restore did not exercise both action classes");

    reset();
    D3D12_RESOURCE_BARRIER to_dest{};
    to_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_dest.Transition.pResource = texture.Get();
    to_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &to_dest);
    for (UINT mip = 0; mip < 2; ++mip) if (restored_mips[mip]) record_texture_upload(list.Get(), texture.Get(), transfers[mip]);
    D3D12_RESOURCE_BARRIER to_source = to_dest;
    to_source.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_source.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &to_source);
    list->CopyBufferRegion(buffer_readback.Get(), 0, buffer.Get(), 0, kBufferBytes);
    for (const auto& t : transfers) record_texture_readback(list.Get(), texture.Get(), t);
    execute();

    const bool restored_buffer_ok = verify_buffer(buffer_readback.Get(), 0x5A);
    bool restored_texture_ok = true;
    for (const auto& t : transfers) restored_texture_ok = restored_texture_ok && verify_texture(t);
    const auto after_restore = arc::dx12::query_memory_budget(adapter.Get());
    const auto debug_errors = debug_error_count(info_queue.Get());

    std::filesystem::create_directories("traces");
    std::ofstream out("traces/global-memory-lab.json", std::ios::trunc);
    out << "{\n"
        << "  \"tiled_resources_tier\": " << static_cast<unsigned>(options.TiledResourcesTier) << ",\n"
        << "  \"synthetic_budget_bytes\": " << synthetic_budget << ",\n"
        << "  \"synthetic_usage_bytes\": " << synthetic_usage << ",\n"
        << "  \"requested_relief_bytes\": " << relief.requested_bytes << ",\n"
        << "  \"planned_relief_bytes\": " << relief.arbitration.planned_bytes << ",\n"
        << "  \"expected_physical_relief_bytes\": " << expected_freed << ",\n"
        << "  \"eviction_actions\": " << eviction_actions << ",\n"
        << "  \"demotion_actions\": " << demotion_actions << ",\n"
        << "  \"restore_headroom_bytes\": " << expected_freed << ",\n"
        << "  \"planned_restore_bytes\": " << restore.arbitration.planned_bytes << ",\n"
        << "  \"make_resident_actions\": " << make_resident_actions << ",\n"
        << "  \"promotion_actions\": " << promotion_actions << ",\n"
        << "  \"dxgi_usage_before_relief_bytes\": " << before_relief->local_usage << ",\n"
        << "  \"dxgi_usage_after_relief_min_bytes\": " << minimum_usage << ",\n"
        << "  \"dxgi_observed_relief_bytes\": " << observed_drop << ",\n"
        << "  \"dxgi_usage_after_restore_bytes\": " << (after_restore ? after_restore->local_usage : 0) << ",\n"
        << "  \"initial_buffer_verified\": " << (initial_buffer_ok ? "true" : "false") << ",\n"
        << "  \"initial_texture_verified\": " << (initial_texture_ok ? "true" : "false") << ",\n"
        << "  \"lower_mip_preserved\": " << (lower_mip_preserved ? "true" : "false") << ",\n"
        << "  \"restored_buffer_verified\": " << (restored_buffer_ok ? "true" : "false") << ",\n"
        << "  \"restored_texture_verified\": " << (restored_texture_ok ? "true" : "false") << ",\n"
        << "  \"debug_error_count\": " << debug_errors << "\n"
        << "}\n";
    out.close();

    CloseHandle(event);
    if (!restored_buffer_ok || !restored_texture_ok || debug_errors) return 1;
    std::cout << "Global memory lab: relief=" << expected_freed << " observed=" << observed_drop
              << " eviction=" << eviction_actions << " demotion=" << demotion_actions
              << " restore=" << restore.arbitration.planned_bytes << '\n';
    return 0;
} catch (const std::exception& e) {
    std::cerr << "global memory lab failed: " << e.what() << '\n';
    return 1;
}
