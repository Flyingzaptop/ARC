#pragma once

#include "arc/events.hpp"
#include "arc/ids.hpp"

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace arc {

enum class ResourceKind : std::uint8_t { Buffer, Texture1D, Texture2D, Texture3D, Unknown };
enum class SafetyClass : std::uint8_t { Unknown, GreenCandidate, Yellow, Red };
enum class Temperature : std::uint8_t { Unknown, Hot, Warm, Cold, Pinned };
enum class ViewType : std::uint8_t { Unknown, Cbv, Srv, Uav, Rtv, Dsv, Sampler };
enum class QueueClass : std::uint8_t { Unknown, Graphics, Compute, Copy };
enum class ResourceAllocationKind : std::uint8_t { Committed, Placed, Reserved, External };

struct HeapCreatePayload final {
    HeapId heap{};
    std::uint64_t size{};
    std::uint64_t backend_properties{};
    std::uint64_t backend_flags{};
};

struct HeapDestroyPayload final { HeapId heap{}; };

struct ResourceCreatePayload final {
    ResourceId resource{};
    HeapId heap{};
    std::uint64_t virtual_bytes{};
    std::uint64_t allocation_bytes{};
    std::uint64_t heap_offset{};
    std::uint64_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint16_t mip_levels{};
    std::uint16_t array_layers{};
    ResourceKind kind{ResourceKind::Unknown};
    ResourceAllocationKind allocation_kind{ResourceAllocationKind::Committed};
    std::uint32_t format{};
    std::uint32_t resource_flags{};
    std::uint8_t plane_count{1};
    std::uint8_t reserved[3]{};
};

struct ResourceDestroyPayload final { ResourceId resource{}; };

struct DescriptorWrittenPayload final {
    DescriptorId descriptor{};
    ResourceId resource{};
    ViewType type{ViewType::Unknown};
    std::uint8_t reserved{};
    std::uint32_t first_mip{};
    std::uint32_t mip_count{}; // UINT32_MAX preserves native all-remaining sentinel
    std::uint32_t first_layer{};
    std::uint32_t layer_count{};
    std::uint32_t format{};
    std::uint64_t buffer_offset{};
    std::uint64_t buffer_bytes{};
};

struct QueueCreatePayload final { QueueId queue{}; QueueClass type{QueueClass::Unknown}; std::uint8_t reserved[7]{}; };
struct CommandListPayload final { CommandId command{}; QueueClass type{QueueClass::Unknown}; std::uint8_t reserved[7]{}; };
struct QueueSubmitPayload final { QueueId queue{}; CommandId command{}; std::uint64_t submission{}; };
struct BarrierPayload final { ResourceId resource{}; CommandId command{}; std::uint32_t before_state{}; std::uint32_t after_state{}; std::uint32_t subresource{}; std::uint32_t reserved{}; };
struct CopyPayload final { ResourceId source{}; ResourceId destination{}; CommandId command{}; std::uint64_t approximate_bytes{}; };
struct FencePayload final { QueueId queue{}; std::uint64_t fence{}; std::uint64_t value{}; };
struct CountersPayload final { CommandId command{}; std::uint64_t draws{}; std::uint64_t indexed_draws{}; std::uint64_t dispatches{}; std::uint64_t indirect{}; };
struct ResourceUsePayload final { CommandId command{}; ResourceId resource{}; std::uint32_t write{}; std::uint32_t reserved{}; };
struct PresentPayload final { std::uint64_t swapchain{}; FrameId frame{}; std::uint32_t sync_interval{}; std::uint32_t flags{}; std::int32_t result{}; std::uint32_t reserved{}; };
struct MemoryBudgetPayload final {
    std::uint64_t local_budget{}; std::uint64_t local_usage{};
    std::uint64_t local_available_for_reservation{}; std::uint64_t local_current_reservation{};
    std::uint64_t nonlocal_budget{}; std::uint64_t nonlocal_usage{};
    std::uint64_t nonlocal_available_for_reservation{}; std::uint64_t nonlocal_current_reservation{};
};

struct HeapRecord final { HeapCreatePayload description{}; bool alive{}; };
struct ViewRecord final { DescriptorWrittenPayload description{}; };
struct QueueRecord final { QueueCreatePayload description{}; std::uint64_t submissions{}; };
struct SubmissionRecord { QueueSubmitPayload description{}; std::uint64_t timestamp_ns{}; FrameId presentation{}; CountersPayload counters{}; };
struct CopyRecord { CopyPayload description{}; QueueId queue{}; std::uint64_t timestamp_ns{}; };
struct CommandRecord { bool closed{}; std::vector<CopyPayload> copies; std::vector<ResourceUsePayload> uses; std::vector<BarrierPayload> barriers; CountersPayload counters{}; };

struct ResourceRecord final {
    ResourceCreatePayload description{};
    std::uint64_t create_timestamp_ns{};
    std::uint64_t destroy_timestamp_ns{};
    std::uint64_t last_read_timestamp_ns{};
    std::uint64_t last_write_timestamp_ns{};
    std::uint64_t read_count{};
    std::uint64_t write_count{};
    SafetyClass safety{SafetyClass::Unknown};
    float safety_confidence{};
    Temperature temperature{Temperature::Unknown};
    FrameId last_used_frame{};
    std::unordered_set<QueueId> queues;
    bool alive{};
    std::uint32_t evidence{}; // historical view bits; descriptor overwrite does not erase history
    std::uint64_t create_sequence{};
    std::uint64_t destroy_sequence{};
    double reuse_interval_frames{};
    std::uint64_t usage_bursts{};
};

// A slow-path backend-neutral view of resource life. It deliberately receives
// copied events from the collector instead of being touched by render threads.
class ResourceGraph final {
public:
    void consume(const Event& event);
    [[nodiscard]] std::optional<ResourceRecord> find(ResourceId id) const;
    [[nodiscard]] std::optional<HeapRecord> find_heap(HeapId id) const;
    [[nodiscard]] std::optional<ViewRecord> find_view(DescriptorId id) const;
    [[nodiscard]] std::size_t resource_count() const noexcept;
    [[nodiscard]] std::uint64_t live_allocation_bytes() const noexcept;
    [[nodiscard]] std::uint64_t live_heap_bytes() const noexcept;
    [[nodiscard]] std::vector<ResourceId> resources_on_heap(HeapId heap) const;
    [[nodiscard]] std::vector<ResourceId> largest_resources(std::size_t limit) const;
    [[nodiscard]] FrameId presentation_frame() const noexcept;
    [[nodiscard]] std::optional<MemoryBudgetPayload> latest_budget() const noexcept;
    void analyze();
    [[nodiscard]] std::vector<ResourceId> alive_at(std::uint64_t timestamp_ns) const;
    [[nodiscard]] std::vector<ResourceId> with_view(ViewType type) const;
    [[nodiscard]] std::vector<ResourceId> unused_for(FrameId presentations) const;
    [[nodiscard]] std::vector<ResourceId> seen_on_queue(QueueId queue) const;
    [[nodiscard]] std::vector<ResourceId> largest_textures(std::size_t limit) const;
    [[nodiscard]] std::uint64_t committed_bytes() const noexcept;
    [[nodiscard]] const auto& resources() const noexcept { return resources_; }
    [[nodiscard]] const auto& submissions() const noexcept { return submissions_; }
    [[nodiscard]] const auto& copies() const noexcept { return copies_; }
    [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }

private:
    std::unordered_map<ResourceId, ResourceRecord> resources_;
    std::unordered_map<HeapId, HeapRecord> heaps_;
    std::unordered_map<DescriptorId, ViewRecord> views_;
    std::unordered_map<QueueId, QueueRecord> queues_;
    std::uint64_t live_allocation_bytes_{};
    std::uint64_t live_heap_bytes_{};
    FrameId presentation_frame_{};
    std::optional<MemoryBudgetPayload> latest_budget_;
    std::unordered_map<CommandId, CommandRecord> commands_;
    std::vector<SubmissionRecord> submissions_;
    std::vector<CopyRecord> copies_;
    std::uint64_t errors_{};
    void use(ResourceId resource, QueueId queue, std::uint64_t timestamp, bool write);
};

}  // namespace arc
