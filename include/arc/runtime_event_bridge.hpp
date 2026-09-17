#pragma once

#include "arc/events.hpp"
#include "arc/live_runtime.hpp"
#include "arc/resource_graph.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace arc {

struct RuntimeEventBridgeMetrics {
    std::uint64_t events{};
    std::uint64_t malformed_events{};
    std::uint64_t resource_uses{};
    std::uint64_t queue_submits{};
    std::uint64_t fence_signals{};
    std::uint64_t completion_updates{};
    std::uint64_t submission_blocks{};
    std::uint64_t completion_releases{};
    std::uint64_t budget_samples{};
    std::uint64_t resources_destroyed{};
    std::uint64_t controller_rejections{};
};

class RuntimeEventBridge final {
public:
    explicit RuntimeEventBridge(LiveRuntimeController& runtime) noexcept : runtime_(runtime) {}

    bool consume(const Event& event);
    void note_queue_completed(QueueId queue, std::uint64_t completed_fence);

    [[nodiscard]] RuntimeEventBridgeMetrics metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::uint64_t logical_epoch() const noexcept { return logical_epoch_; }
    [[nodiscard]] std::uint64_t presentation_frame() const noexcept { return presentation_frame_; }

private:
    struct SignaledBatch {
        std::uint64_t fence_value{};
        std::vector<ResourceId> resources{}; // one entry per submission-level in-flight block
    };

    void append_use(CommandId command, ResourceId resource);
    bool queue_submission(QueueId queue, const std::vector<ResourceId>& uses);
    bool flush_signal(QueueId queue, std::uint64_t fence_value);

    LiveRuntimeController& runtime_;
    std::unordered_map<CommandId, std::vector<ResourceId>> command_uses_{};
    std::unordered_map<QueueId, std::vector<ResourceId>> pending_uses_by_queue_{};
    std::unordered_map<QueueId, std::vector<std::vector<ResourceId>>> pending_blocks_by_queue_{};
    std::unordered_map<QueueId, std::vector<SignaledBatch>> signaled_by_queue_{};
    std::unordered_map<QueueId, std::uint64_t> completed_by_queue_{};
    RuntimeEventBridgeMetrics metrics_{};
    std::uint64_t logical_epoch_{};
    std::uint64_t presentation_frame_{};
};

}  // namespace arc
