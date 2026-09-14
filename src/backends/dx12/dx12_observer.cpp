#include "arc/dx12_observer.hpp"

#include "arc/clock.hpp"
#include "arc/resource_graph.hpp"
#include "arc/footprint.hpp"

#include <cstring>
#include <optional>

namespace arc::dx12 {
namespace {

ResourceKind resource_kind(const D3D12_RESOURCE_DIMENSION dimension) noexcept {
    switch (dimension) {
    case D3D12_RESOURCE_DIMENSION_BUFFER: return ResourceKind::Buffer;
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return ResourceKind::Texture1D;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: return ResourceKind::Texture2D;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return ResourceKind::Texture3D;
    default: return ResourceKind::Unknown;
    }
}

std::uint64_t logical_bytes(const D3D12_RESOURCE_DESC& desc) noexcept {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return desc.Width;
    }
    // The allocation query below is the authoritative host estimate. This is
    // deliberately just a conservative logical placeholder until format/mip
    // footprint accounting is added in the descriptor and subresource work.
    arc::TexelLayout layout{};
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: break;
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC4_UNORM: layout = {4, 4, 8}; break;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM: layout = {4, 4, 16}; break;
    default: return 0; // Unsupported logical format is explicitly unknown.
    }
    auto mips = desc.MipLevels;
    if (!mips) { auto size = (std::max)(desc.Width, static_cast<UINT64>(desc.Height)); do { ++mips; size >>= 1; } while (size); }
    auto w = desc.Width; UINT64 h = desc.Height;
    UINT64 depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
    const UINT64 layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    UINT64 total{};
    for (unsigned mip = 0; mip < mips; ++mip) {
        total += ((w + layout.block_width - 1) / layout.block_width) * ((h + layout.block_height - 1) / layout.block_height) * layout.bytes_per_block * depth * layers * desc.SampleDesc.Count;
        w = (std::max)(1ULL, w / 2); h = (std::max)(1ULL, h / 2); depth = (std::max)(1ULL, depth / 2);
    }
    return total;
}

}  // namespace

Observer::Observer(EventRing& events, IdAllocator& ids) noexcept : events_(events), ids_(ids) {}

ResourceId Observer::observe_committed_resource(
    ID3D12Device* device, const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept {
    return observe_resource(device, 0, 0, ResourceAllocationKind::Committed, description, resource);
}

HeapId Observer::observe_heap(const D3D12_HEAP_DESC& description, ID3D12Heap* heap) noexcept {
    if (heap == nullptr) { return 0; }
    const HeapId id = ids_.next();
    const HeapCreatePayload payload{
        .heap = id,
        .size = description.SizeInBytes,
        .backend_properties = static_cast<std::uint64_t>(description.Properties.Type),
        .backend_flags = static_cast<std::uint64_t>(description.Flags),
    };
    return emit(EventType::HeapCreated, &payload, sizeof(payload)) ? id : 0;
}

void Observer::observe_heap_destroyed(const HeapId heap) noexcept {
    const HeapDestroyPayload payload{.heap = heap};
    (void)emit(EventType::HeapDestroyed, &payload, sizeof(payload));
}

ResourceId Observer::observe_placed_resource(ID3D12Device* device, const HeapId heap, const std::uint64_t offset,
                                              const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept {
    return observe_resource(device, heap, offset, ResourceAllocationKind::Placed, description, resource);
}

ResourceId Observer::observe_reserved_resource(const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept {
    return observe_resource(nullptr, 0, 0, ResourceAllocationKind::Reserved, description, resource);
}

ResourceId Observer::observe_resource(ID3D12Device* device, const HeapId heap, const std::uint64_t offset,
                                      const ResourceAllocationKind kind, const D3D12_RESOURCE_DESC& description,
                                      ID3D12Resource* resource) noexcept {
    if (resource == nullptr || (kind != ResourceAllocationKind::Reserved && device == nullptr)) {
        return 0;
    }
    const auto allocation = device == nullptr ? D3D12_RESOURCE_ALLOCATION_INFO{} :
        device->GetResourceAllocationInfo(0, 1, &description);
    const ResourceId id = ids_.next();
    const ResourceCreatePayload payload{
        .resource = id,
        .heap = heap,
        .virtual_bytes = logical_bytes(description),
        .allocation_bytes = kind == ResourceAllocationKind::Reserved ? 0 : allocation.SizeInBytes,
        .heap_offset = offset,
        .width = static_cast<std::uint32_t>(description.Width > UINT32_MAX ? UINT32_MAX : description.Width),
        .height = description.Height,
        .depth = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? description.DepthOrArraySize : 1U,
        .mip_levels = description.MipLevels,
        .array_layers = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::uint16_t{1} : description.DepthOrArraySize,
        .kind = resource_kind(description.Dimension),
        .allocation_kind = kind,
        .format = static_cast<std::uint32_t>(description.Format),
        .resource_flags = static_cast<std::uint32_t>(description.Flags),
    };
    return emit(EventType::ResourceCreated, &payload, sizeof(payload)) ? id : 0;
}

void Observer::observe_resource_destroyed(const ResourceId resource) noexcept {
    const ResourceDestroyPayload payload{.resource = resource};
    (void)emit(EventType::ResourceDestroyed, &payload, sizeof(payload));
}

bool Observer::emit(const EventType type, const void* payload, const std::uint32_t bytes) noexcept {
    if (bytes > kMaxEventPayloadBytes) {
        return false;
    }
    Event event{};
    event.header.timestamp_ns = monotonic_time_ns();
    event.header.thread_id = GetCurrentThreadId();
    event.header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    event.header.type = type;
    event.header.payload_bytes = bytes;
    std::memcpy(event.payload.data(), payload, bytes);
    return events_.try_emit(event);
}

std::optional<MemoryBudgetPayload> query_memory_budget(IDXGIAdapter3* adapter) noexcept {
    if (adapter == nullptr) { return std::nullopt; }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        return std::nullopt;
    }
    return MemoryBudgetPayload{
        .local_budget = local.Budget,
        .local_usage = local.CurrentUsage,
        .local_available_for_reservation = local.AvailableForReservation,
        .local_current_reservation = local.CurrentReservation,
        .nonlocal_budget = nonlocal.Budget,
        .nonlocal_usage = nonlocal.CurrentUsage,
        .nonlocal_available_for_reservation = nonlocal.AvailableForReservation,
        .nonlocal_current_reservation = nonlocal.CurrentReservation,
    };
}

}  // namespace arc::dx12
