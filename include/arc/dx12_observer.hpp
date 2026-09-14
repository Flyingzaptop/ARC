#pragma once

#include "arc/event_ring.hpp"
#include "arc/ids.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdint>

namespace arc::dx12 {

// Read-only adapter surface for integrations that already own a D3D12 device.
// It observes object creation after the application created it; it never
// substitutes descriptors, transitions, or resource contents.
class Observer final {
public:
    Observer(EventRing& events, IdAllocator& ids) noexcept;

    [[nodiscard]] ResourceId observe_committed_resource(
        ID3D12Device* device,
        const D3D12_RESOURCE_DESC& description,
        ID3D12Resource* resource) noexcept;
    void observe_resource_destroyed(ResourceId resource) noexcept;

private:
    [[nodiscard]] bool emit(EventType type, const void* payload, std::uint32_t bytes) noexcept;

    EventRing& events_;
    IdAllocator& ids_;
    std::atomic<std::uint64_t> sequence_{1};
};

}  // namespace arc::dx12
