#include "arc/dx12_observer.hpp"

#include "arc/clock.hpp"
#include "arc/resource_graph.hpp"

#include <cstring>

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
    return 0;
}

}  // namespace

Observer::Observer(EventRing& events, IdAllocator& ids) noexcept : events_(events), ids_(ids) {}

ResourceId Observer::observe_committed_resource(
    ID3D12Device* device, const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept {
    if (device == nullptr || resource == nullptr) {
        return 0;
    }
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &description);
    const ResourceId id = ids_.next();
    const ResourceCreatePayload payload{
        .resource = id,
        .virtual_bytes = logical_bytes(description),
        .allocation_bytes = allocation.SizeInBytes,
        .width = static_cast<std::uint32_t>(description.Width > UINT32_MAX ? UINT32_MAX : description.Width),
        .height = description.Height,
        .depth = description.DepthOrArraySize,
        .mip_levels = description.MipLevels,
        .array_layers = description.DepthOrArraySize,
        .kind = resource_kind(description.Dimension),
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
    event.header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    event.header.type = type;
    event.header.payload_bytes = bytes;
    std::memcpy(event.payload.data(), payload, bytes);
    return events_.try_emit(event);
}

}  // namespace arc::dx12
