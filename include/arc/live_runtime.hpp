#pragma once

#include "arc/memory_planner.hpp"
#include "arc/resource_graph.hpp"
#include "arc/transition_predictor.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace arc {

struct LiveRuntimeConfig {
    ResidencyPolicyConfig residency{};
    TextureQualityPolicyConfig textures{};
    GlobalMemoryPlannerConfig planner{};
    TransitionPredictorConfig transitions{};
    double minimum_transition_prefetch_confidence{0.40};
    std::uint32_t max_transition_prefetch_actions{4};
    std::uint64_t minimum_restore_reserve_bytes{};
    std::uint64_t max_restore_bytes_per_tick{256ull * 1024ull * 1024ull};
};

struct LiveRuntimeMetrics {
    std::uint64_t observed_uses{};
    std::uint64_t controlled_uses{};
    std::uint64_t rejected_controlled_uses{};
    std::uint64_t pressure_plans{};
    std::uint64_t restore_plans{};
    std::uint64_t transition_prefetch_plans{};
    std::uint64_t transition_prefetch_actions{};
    std::uint64_t resources_registered{};
    std::uint64_t resources_unregistered{};
    std::uint64_t plan_resolve_failures{};
    std::uint64_t inflight_blocks_started{};
    std::uint64_t inflight_blocks_completed{};
};

struct LiveRuntimePlan {
    std::uint64_t epoch{};
    PressureState pressure{PressureState::Normal};
    std::uint64_t local_budget{};
    std::uint64_t local_usage{};
    std::uint64_t restore_headroom{};
    GlobalMemoryPlan pressure_relief{};
    std::vector<ResidencyAction> transition_prefetch{};
    GlobalMemoryRestorePlan restore{};
};

using LiveRuntimeResolvedAction = std::variant<ResidencyAction, TextureQualityAction>;

class LiveRuntimeController final {
public:
    explicit LiveRuntimeController(LiveRuntimeConfig config = {});

    bool register_controlled_resource(ResidencyObject object);
    bool register_controlled_texture(TextureQualityObject object);
    bool unregister_resource(ResourceId resource) noexcept;

    bool note_use(
        ResourceId resource,
        std::uint64_t epoch,
        QueueId queue,
        std::uint64_t submitted_fence,
        std::uint64_t completed_fence);
    bool note_completed(ResourceId resource, std::uint64_t completed_fence);

    // Event-stream adapters call mark_inflight() at QueueSubmit and release it
    // only after the associated signaled fence is actually completed. Counts
    // compose across multiple submissions and queues, so any outstanding GPU
    // use makes pressure/restore resolution fail closed for that resource.
    bool mark_inflight(ResourceId resource) noexcept;
    bool release_inflight(ResourceId resource, std::uint64_t completed_fence) noexcept;
    [[nodiscard]] std::uint32_t inflight_count(ResourceId resource) const noexcept;

    void update_budget(const MemoryBudgetPayload& budget);
    void reset_sequence_context() noexcept { transitions_.reset_context(); }

    [[nodiscard]] LiveRuntimePlan plan(std::uint64_t epoch);

    [[nodiscard]] std::optional<std::vector<LiveRuntimeResolvedAction>> resolve_pressure_actions(
        const GlobalMemoryPlan& plan,
        std::uint64_t epoch) const;
    [[nodiscard]] std::optional<std::vector<LiveRuntimeResolvedAction>> resolve_restore_actions(
        const GlobalMemoryRestorePlan& plan,
        std::uint64_t epoch) const;

    bool begin_residency_action(const ResidencyAction& action, std::uint64_t epoch, bool demand_miss = false);
    bool complete_make_resident(ResidencyId object);
    bool apply_texture_action(const TextureQualityAction& action, std::uint64_t epoch);
    void record_resolve_failure() noexcept { ++metrics_.plan_resolve_failures; }

    [[nodiscard]] bool controlled(ResourceId resource) const noexcept;
    [[nodiscard]] LiveRuntimeMetrics metrics() const noexcept { return metrics_; }
    [[nodiscard]] const ResourceTransitionPredictor& transitions() const noexcept { return transitions_; }
    [[nodiscard]] const ResidencyGovernor& residency() const noexcept { return residency_; }
    [[nodiscard]] const TextureQualityGovernor& textures() const noexcept { return textures_; }
    [[nodiscard]] const LiveRuntimeConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] std::uint64_t restore_headroom() const noexcept;
    [[nodiscard]] std::vector<ResidencyAction> transition_prefetch(std::uint64_t epoch, std::uint64_t headroom);
    [[nodiscard]] bool externally_inflight(ResourceId resource) const noexcept;

    LiveRuntimeConfig config_{};
    ResidencyGovernor residency_{};
    TextureQualityGovernor textures_{};
    GlobalMemoryPlanner planner_{};
    ResourceTransitionPredictor transitions_{};
    std::unordered_map<ResourceId, ResidencyId> residency_by_resource_{};
    std::unordered_map<ResidencyId, ResourceId> resource_by_residency_{};
    std::unordered_map<ResourceId, TextureQualityId> texture_by_resource_{};
    std::unordered_map<TextureQualityId, ResourceId> resource_by_texture_{};
    std::unordered_map<ResourceId, std::uint32_t> inflight_counts_{};
    LiveRuntimeMetrics metrics_{};
};

}  // namespace arc
