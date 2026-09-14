#pragma once

#include "arc/events.hpp"
#include "arc/ids.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace arc {

enum class ResourceKind : std::uint8_t { Buffer, Texture1D, Texture2D, Texture3D, Unknown };
enum class SafetyClass : std::uint8_t { Unknown, GreenCandidate, Yellow, Red };

struct ResourceCreatePayload final {
    ResourceId resource{};
    HeapId heap{};
    std::uint64_t virtual_bytes{};
    std::uint64_t allocation_bytes{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint16_t mip_levels{};
    std::uint16_t array_layers{};
    ResourceKind kind{ResourceKind::Unknown};
    std::uint8_t reserved[7]{};
};
static_assert(sizeof(ResourceCreatePayload) == 56);

struct ResourceDestroyPayload final { ResourceId resource{}; };
static_assert(sizeof(ResourceDestroyPayload) == 8);

struct ResourceRecord final {
    ResourceCreatePayload description{};
    std::uint64_t create_timestamp_ns{};
    std::uint64_t destroy_timestamp_ns{};
    std::uint64_t last_read_timestamp_ns{};
    std::uint64_t last_write_timestamp_ns{};
    std::uint64_t read_count{};
    std::uint64_t write_count{};
    SafetyClass safety{SafetyClass::Unknown};
    bool alive{};
};

// A slow-path backend-neutral view of resource life. It deliberately receives
// copied events from the collector instead of being touched by render threads.
class ResourceGraph final {
public:
    void consume(const Event& event);
    [[nodiscard]] std::optional<ResourceRecord> find(ResourceId id) const;
    [[nodiscard]] std::size_t resource_count() const noexcept;
    [[nodiscard]] std::uint64_t live_allocation_bytes() const noexcept;

private:
    std::unordered_map<ResourceId, ResourceRecord> resources_;
    std::uint64_t live_allocation_bytes_{};
};

}  // namespace arc
