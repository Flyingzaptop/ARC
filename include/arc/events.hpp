#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace arc {

enum class EventType : std::uint16_t {
    SessionStart = 1,
    SessionEnd,
    AdapterObserved,
    DeviceCreated,
    HeapCreated,
    HeapDestroyed,
    ResourceCreated,
    ResourceDestroyed,
    DescriptorHeapCreated,
    DescriptorWritten,
    CommandQueueCreated,
    CommandListCreated,
    CommandListReset,
    CommandListClosed,
    Barrier,
    CopyResource,
    CopyBuffer,
    CopyTexture,
    Draw,
    DrawIndexed,
    Dispatch,
    ExecuteIndirect,
    QueueSubmit,
    FenceSignal,
    FenceWait,
    SwapchainCreated,
    Present,
    MemoryBudgetSample,
    TelemetrySample,
    TraceOverflow,
    DiagnosticError,
    ResolveSubresource,
    CommandCounters,
    ResourceUse,
};

struct EventHeader final {
    std::uint64_t timestamp_ns{};
    std::uint64_t sequence{};
    std::uint32_t thread_id{};
    EventType type{};
    std::uint16_t flags{};
    std::uint32_t payload_bytes{};
};
static_assert(std::is_trivially_copyable_v<EventHeader>);
static_assert(sizeof(EventHeader) == 32);

constexpr std::size_t kMaxEventPayloadBytes = 224;

// Fixed-size event envelope avoids allocation in render/API call paths.
struct Event final {
    EventHeader header{};
    std::array<std::byte, kMaxEventPayloadBytes> payload{};
};
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace arc
