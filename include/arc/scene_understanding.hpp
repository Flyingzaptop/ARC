#pragma once

#include "arc/resource_semantics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace arc {

struct SceneResourceUsageCheckpoint {
    std::uint64_t reads{};
    std::uint64_t writes{};
    std::uint64_t usage_bursts{};
    std::uint64_t reuse_gap_frames_sum{};
    std::uint64_t reuse_gap_samples{};
    std::unordered_map<QueueId, std::uint64_t> queue_use_counts;
};

struct SceneObservationCheckpoint {
    FrameId frame{};
    std::size_t submission_count{};
    std::size_t copy_count{};
    bool valid{};
    GraphWorkloadTotals workload{};
    std::uint64_t resource_history_generation{};
    std::unordered_map<ResourceId, SceneResourceUsageCheckpoint> resources;
};

struct SceneSemanticSignature {
    // False means retained resource/command history cannot cover this window.
    // Workload checkpoint deltas remain exact even after command-history pruning.
    bool history_complete{true};
    FrameId frame{};
    FrameId window_frames{};
    std::uint64_t active_resources{};
    std::uint64_t known_resources{};
    std::uint64_t active_bytes{};
    std::uint64_t known_bytes{};
    float coverage{};
    float mean_confidence{};
    float read_fraction{};
    float write_fraction{};
    float multi_queue_fraction{};
    std::uint64_t submissions{};
    std::uint64_t draws{};
    std::uint64_t indexed_draws{};
    std::uint64_t dispatches{};
    std::uint64_t indirect{};
    std::uint64_t draw_items{};
    std::uint64_t dispatch_groups{};
    std::uint64_t copies{};
    float resource_accesses_per_frame{};
    float draw_calls_per_frame{};
    float draw_items_per_frame{};
    float dispatches_per_frame{};
    float dispatch_groups_per_frame{};
    float indirect_per_frame{};
    float submissions_per_frame{};
    float copies_per_frame{};
    std::array<float, kInferredResourceSemanticCount> resource_fractions{};
    std::array<float, kInferredResourceSemanticCount> byte_fractions{};
};

struct SceneSemanticComparison {
    float distance{};
    float similarity{1.0F};
    float confidence{};
    bool same_scene{};
};

struct SceneClusterAssignment {
    std::uint32_t cluster{};
    float distance{};
    float confidence{};
    bool created{};
};

struct SceneUnderstandingConfig {
    FrameId active_window_frames{12};
    float minimum_resource_confidence{0.55F};
    float same_scene_distance{0.22F};
    float cluster_distance{0.22F};
    float centroid_learning_rate{0.20F};
};

class SceneUnderstandingInferencer final {
public:
    explicit SceneUnderstandingInferencer(SceneUnderstandingConfig config = {}) noexcept;

    [[nodiscard]] SceneObservationCheckpoint checkpoint(const ResourceGraph& graph) const;
    [[nodiscard]] SceneSemanticSignature summarize(const ResourceGraph& graph) const;
    [[nodiscard]] SceneSemanticSignature summarize(
        const ResourceGraph& graph,
        const SceneObservationCheckpoint& checkpoint) const;
    [[nodiscard]] SceneSemanticComparison compare(
        const SceneSemanticSignature& a,
        const SceneSemanticSignature& b) const noexcept;

    [[nodiscard]] const SceneUnderstandingConfig& config() const noexcept { return config_; }

private:
    SceneUnderstandingConfig config_{};
};

class SceneSemanticClusterer final {
public:
    explicit SceneSemanticClusterer(SceneUnderstandingConfig config = {}) noexcept;

    [[nodiscard]] SceneClusterAssignment observe(const SceneSemanticSignature& signature);
    void reset() noexcept;

    [[nodiscard]] std::size_t cluster_count() const noexcept { return centroids_.size(); }
    [[nodiscard]] const std::vector<SceneSemanticSignature>& centroids() const noexcept { return centroids_; }

private:
    SceneUnderstandingConfig config_{};
    SceneUnderstandingInferencer inferencer_{};
    std::vector<SceneSemanticSignature> centroids_;
    std::vector<std::uint64_t> observations_;
};

} // namespace arc
