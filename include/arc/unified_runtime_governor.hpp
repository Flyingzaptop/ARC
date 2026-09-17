#pragma once

#include "arc/adaptive_quality_controller.hpp"
#include "arc/global_action_arbiter.hpp"
#include "arc/quality_admission.hpp"
#include "arc/runtime_coordinator.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace arc {

enum class UnifiedGovernorPath : std::uint8_t {
    None,
    Memory,
    Quality,
    Combined,
    Restore,
    ObserveOnly,
    PlanOnly,
    CircuitOpen,
};

struct UnifiedRuntimeGovernorConfig {
    AdaptiveQualityControllerConfig quality{};
    QualityAdmissionConfig admission{};
    GlobalActionArbiterConfig arbitration{};
    double memory_restore_ceiling{0.86};
    std::uint32_t max_consecutive_quality_failures{3};
    bool enable_memory{true};
    bool enable_quality{true};
};

struct UnifiedRuntimeGovernorMetrics {
    std::uint64_t ticks{};
    std::uint64_t quality_plans{};
    std::uint64_t quality_actions_attempted{};
    std::uint64_t quality_actions_executed{};
    std::uint64_t quality_backend_failures{};
    std::uint64_t quality_circuit_trips{};
    std::uint64_t pending_effects_resolved{};
    std::uint64_t memory_priority_ticks{};
    std::uint64_t combined_ticks{};
    std::uint64_t admission_queries{};
    std::uint64_t admission_reductions{};
};

struct UnifiedRuntimeTickResult {
    UnifiedGovernorPath path{UnifiedGovernorPath::None};
    RuntimeTickResult memory{};
    QualityDecision quality{};
    GlobalArbitrationDecision arbitration{};
    RuntimeBackendStatus quality_backend_status{RuntimeBackendStatus::Success};
    bool quality_attempted{};
    bool quality_executed{};
    bool pending_effect_resolved{};
    bool quality_circuit_open{};
    double memory_pressure{};
    std::size_t registered_quality_profiles{};
};

// Production closed-loop governor. Memory/residency and graphics quality are
// planned together and passed through a global arbiter before mutation. The
// class does not inspect private game state: a host registers safe quality
// profiles and binds physical mutations through RuntimeMutationBackend.
class UnifiedRuntimeGovernor final {
public:
    UnifiedRuntimeGovernor(
        LiveRuntimeController& runtime,
        RuntimeCoordinator& coordinator,
        RuntimeMutationBackend* backend = nullptr,
        UnifiedRuntimeGovernorConfig config = {});

    void set_backend(RuntimeMutationBackend* backend) noexcept { backend_ = backend; }
    void reset_quality_circuit() noexcept;

    bool register_quality_profile(QualityResourceProfile profile);
    bool unregister_quality_profile(std::uint64_t id) noexcept;
    bool update_quality_importance(std::uint64_t id, ResourceImportanceSample importance) noexcept;
    bool update_quality_levels(std::uint64_t id, std::vector<QualityLevelStep> levels) noexcept;

    [[nodiscard]] QualityAdmissionDecision admit(
        const QualityAdmissionResource& resource,
        const FrameBudgetSample& frame);

    [[nodiscard]] UnifiedRuntimeTickResult tick(
        std::uint64_t epoch,
        const FrameBudgetSample& frame);

    [[nodiscard]] AdaptiveQualityController& quality() noexcept { return quality_; }
    [[nodiscard]] const AdaptiveQualityController& quality() const noexcept { return quality_; }
    [[nodiscard]] const QualityAdmissionController& admission() const noexcept { return admission_; }
    [[nodiscard]] const GlobalActionArbiter& arbiter() const noexcept { return arbiter_; }
    [[nodiscard]] UnifiedRuntimeGovernorMetrics metrics() const noexcept { return metrics_; }
    [[nodiscard]] bool quality_circuit_open() const noexcept { return quality_circuit_open_; }
    [[nodiscard]] std::size_t registered_quality_profiles() const noexcept { return profiles_.size(); }

private:
    struct PendingEffect {
        QualityActionCandidate action{};
        QualityDecisionKind kind{QualityDecisionKind::None};
        double before_frame_ms{};
    };

    [[nodiscard]] std::vector<QualityActionCandidate> candidates() const;
    [[nodiscard]] double memory_pressure(const FrameBudgetSample& frame) const noexcept;
    [[nodiscard]] bool execute_quality(
        const QualityDecision& decision,
        const FrameBudgetSample& frame,
        UnifiedRuntimeTickResult& result);
    void record_quality_failure() noexcept;

    LiveRuntimeController& runtime_;
    RuntimeCoordinator& coordinator_;
    RuntimeMutationBackend* backend_{};
    UnifiedRuntimeGovernorConfig config_{};
    AdaptiveQualityController quality_{};
    QualityAdmissionController admission_{};
    GlobalActionArbiter arbiter_{};
    std::unordered_map<std::uint64_t, QualityResourceProfile> profiles_{};
    std::optional<PendingEffect> pending_{};
    std::uint32_t consecutive_quality_failures_{};
    bool quality_circuit_open_{};
    UnifiedRuntimeGovernorMetrics metrics_{};
};

} // namespace arc
