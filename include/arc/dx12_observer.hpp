#pragma once

#include "arc/event_ring.hpp"
#include "arc/ids.hpp"
#include "arc/resource_graph.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <optional>

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
    [[nodiscard]] HeapId observe_heap(const D3D12_HEAP_DESC& description, ID3D12Heap* heap) noexcept;
    void observe_heap_destroyed(HeapId heap) noexcept;
    [[nodiscard]] ResourceId observe_placed_resource(
        ID3D12Device* device, HeapId heap, std::uint64_t offset,
        const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept;
    [[nodiscard]] ResourceId observe_reserved_resource(
        const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept;
    void observe_resource_destroyed(ResourceId resource) noexcept;
    // Explicit integration hooks: caller reports a successful API operation.
    // One Observer is confined to one producer thread/ring.
    template<class T> bool observe(EventType type, const T& payload) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= kMaxEventPayloadBytes);
        return emit(type, &payload, sizeof(T));
    }

private:
    [[nodiscard]] bool emit(EventType type, const void* payload, std::uint32_t bytes) noexcept;
    [[nodiscard]] ResourceId observe_resource(
        ID3D12Device* device, HeapId heap, std::uint64_t offset,
        ResourceAllocationKind kind, const D3D12_RESOURCE_DESC& description, ID3D12Resource* resource) noexcept;

    EventRing& events_;
    IdAllocator& ids_;
    std::atomic<std::uint64_t> sequence_{1};
};

// Snapshot only. DXGI reports current process accounting; physical VRAM is not
// substituted for the dynamic operating-system budget.
[[nodiscard]] std::optional<MemoryBudgetPayload> query_memory_budget(IDXGIAdapter3* adapter) noexcept;

}  // namespace arc::dx12
