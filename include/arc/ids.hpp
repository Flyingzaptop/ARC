#pragma once

#include <atomic>
#include <cstdint>

namespace arc {

using ResourceId = std::uint64_t;
using HeapId = std::uint64_t;
using DescriptorId = std::uint64_t;
using QueueId = std::uint64_t;
using CommandId = std::uint64_t;
using FrameId = std::uint64_t;
using SessionId = std::uint64_t;

// IDs are session-local and are never reused. Backends must not use API
// pointers as durable identity.
class IdAllocator final {
public:
    [[nodiscard]] std::uint64_t next() noexcept {
        return next_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> next_{1};
};

}  // namespace arc
