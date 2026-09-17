#include "arc/unified_runtime_governor.hpp"

#include <algorithm>
#include <cmath>

namespace arc {

UnifiedRuntimeGovernor::UnifiedRuntimeGovernor(
    LiveRuntimeController& runtime,
    RuntimeCoordinator& coordinator,
    RuntimeMutationBackend* backend,
    UnifiedRuntimeGovernorConfig config)
    : runtime_(runtime),
      coordinator_(coordinator),
      backend_(backend),
      config_(config),
      quality_(config.quality),
      admission_(config.admission) {
    config_.memory_pressure_enter = std::clamp(config_.memory_pressure_enter, 0.0, 1.0);
    config_.memory_pressure_emergency = std::clamp(
        std::max(config_.memory_pressure_enter, config_.memory_pressure_emergency), 0.0, 1.0);
    config_.memory_restore_ceiling = std::clamp(
        std::min(config_.memory_pressure_enter, config_.memory_restore_ceiling), 0.0, 1.0);
    config_.max_consecutive_quality_failures =
        std::max<std::uint32_t>(1, config_.max_consecutive_quality_failures);
}

void UnifiedRuntimeGovernor::reset_quality_circuit() noexcept {
    consecutive_quality_failures_ = 0;
    quality_circuit_open_ = false;
}

bool UnifiedRuntimeGovernor::register_quality_profile(QualityResourceProfile profile) {
    if (profile.id == 0 || profile.levels.empty() || profile.temporal_assist ||
        profile.domain == QualityDomain::Temporal || profiles_.contains(profile.id)) {
        return false;
    }
    return profiles_.emplace(profile.id, std::move(profile)).second;
}

bool UnifiedRuntimeGovernor::unregister_quality_profile(std::uint64_t id) noexcept {
    if (!id) return false;
    // Do not silently remove a profile while its action is pending measurement.
    if (pending_ && pending_->action.id == id) return false;
    for (const auto& active : quality_.active_actions()) {
        if (active.id == id) return false;
    }
    return profiles_.erase(id) != 0;
}

bool UnifiedRuntimeGovernor::update_quality_importance(
    std::uint64_t id,
    ResourceImportanceSample importance) noexcept {
    const auto it = profiles_.find(id);
    if (it == profiles_.end()) return false;
    it->second.importance = importance;
    return true;
}

bool UnifiedRuntimeGovernor::update_quality_levels(
    std::uint64_t id,
    std::vector<QualityLevelStep> levels) noexcept {
    const auto it = profiles_.find(id);
    if (it == profiles_.end() || levels.empty()) return false;
    for (const auto& active : quality_.active_actions()) {
        if (active.id == id) return false;
    }
    if (pending_ && pending_->action.id == id) return false;
    it->second.levels = std::move(levels);
    return true;
}

QualityAdmissionDecision UnifiedRuntimeGovernor::admit(
    const QualityAdmissionResource& resource,
    const FrameBudgetSample& frame) {
    ++metrics_.admission_queries;
    auto decision = admission_.decide(resource, frame);
    if (decision.reduced) ++metrics_.admission_reductions;
    return decision;
}

std::vector<QualityActionCandidate> UnifiedRuntimeGovernor::candidates() const {
    std::vector<QualityActionCandidate> out;
    for (const auto& [_, profile] : profiles_) {
        auto built = QualityCandidateFactory::build(profile);
        out.insert(out.end(), built.begin(), built.end());
    }
    return out;
}

double UnifiedRuntimeGovernor::memory_pressure(const FrameBudgetSample& frame) const noexcept {
    if (frame.local_budget_bytes == 0) return 0.0;
    return std::clamp(
        static_cast<double>(frame.local_usage_bytes) /
            static_cast<double>(frame.local_budget_bytes),
        0.0,
        4.0);
}

void UnifiedRuntimeGovernor::record_quality_failure() noexcept {
    ++metrics_.quality_backend_failures;
    if (consecutive_quality_failures_ < (std::numeric_limits<std::uint32_t>::max)()) {
        ++consecutive_quality_failures_;
    }
    if (consecutive_quality_failures_ >= config_.max_consecutive_quality_failures && !quality_circuit_open_) {
        quality_circuit_open_ = true;
        ++metrics_.quality_circuit_trips;
    }
}

bool UnifiedRuntimeGovernor::execute_quality(
    const QualityDecision& decision,
    const FrameBudgetSample& frame,
    UnifiedRuntimeTickResult& result) {
    if (decision.kind == QualityDecisionKind::None || decision.plan.actions.empty()) return false;
    if (pending_) return false;
    if (quality_circuit_open_ || coordinator_.circuit_open()) return false;
    if (coordinator_.requested_mode() != RuntimeMode::Controlled || !backend_) return false;

    const auto& action = decision.plan.actions.front();
    result.quality_attempted = true;
    ++metrics_.quality_actions_attempted;

    RuntimeBackendStatus status = RuntimeBackendStatus::Unsupported;
    if (decision.kind == QualityDecisionKind::Degrade) status = backend_->apply_quality(action);
    else if (decision.kind == QualityDecisionKind::Restore) status = backend_->restore_quality(action);
    result.quality_backend_status = status;

    if (status != RuntimeBackendStatus::Success) {
        quality_.note_action_applied(action, decision.kind, frame.frame_ms, frame.frame_ms, false);
        record_quality_failure();
        return false;
    }

    consecutive_quality_failures_ = 0;
    pending_ = PendingEffect{action, decision.kind, frame.frame_ms};
    result.quality_executed = true;
    ++metrics_.quality_actions_executed;
    return true;
}

UnifiedRuntimeTickResult UnifiedRuntimeGovernor::tick(
    std::uint64_t epoch,
    const FrameBudgetSample& frame) {
    UnifiedRuntimeTickResult result{};
    ++metrics_.ticks;
    result.registered_quality_profiles = profiles_.size();
    result.memory_pressure = memory_pressure(frame);

    // Close the feedback loop using the first observation after a successful
    // physical mutation. This makes online effect learning causal rather than
    // crediting the optimizer's predicted gain to itself.
    if (pending_) {
        quality_.note_action_applied(
            pending_->action,
            pending_->kind,
            pending_->before_frame_ms,
            frame.frame_ms,
            true);
        pending_.reset();
        result.pending_effect_resolved = true;
        ++metrics_.pending_effects_resolved;
    }

    const bool frame_over = std::isfinite(frame.frame_ms) && std::isfinite(frame.target_frame_ms) &&
        frame.target_frame_ms > 0.0 && frame.frame_ms > frame.target_frame_ms;
    const bool memory_enter = result.memory_pressure >= config_.memory_pressure_enter;
    const bool memory_emergency = result.memory_pressure >= config_.memory_pressure_emergency;

    // One global loop owns memory restoration policy too. If the frame is
    // already over budget, normal-memory MakeResident/prefetch restoration is
    // planned but not physically executed on this tick. Capacity pressure still
    // gets priority and may execute relief immediately.
    if (config_.enable_memory) {
        const auto requested = coordinator_.requested_mode();
        const bool suppress_restore_execution =
            requested == RuntimeMode::Controlled && frame_over && !memory_enter;
        if (suppress_restore_execution) coordinator_.set_mode(RuntimeMode::PlanOnly);
        result.memory = coordinator_.tick(epoch);
        if (suppress_restore_execution) coordinator_.set_mode(requested);
        if (memory_emergency) ++metrics_.memory_priority_ticks;
    }

    if (config_.enable_quality && !profiles_.empty()) {
        result.quality = quality_.tick(frame, candidates());
        if (result.quality.kind != QualityDecisionKind::None) ++metrics_.quality_plans;
    }

    bool allow_quality_execution = config_.enable_quality;
    if (result.quality.kind == QualityDecisionKind::Restore &&
        result.memory_pressure > config_.memory_restore_ceiling) {
        allow_quality_execution = false;
    }

    // Under capacity emergency, give physical memory relief one observation to
    // settle before spending more visual quality. If memory cannot make
    // progress, a generic quality action that also frees bytes may proceed.
    if (memory_emergency && result.memory.executed_actions != 0) {
        allow_quality_execution = false;
    }
    if (memory_emergency && result.memory.executed_actions == 0 &&
        result.quality.kind == QualityDecisionKind::Degrade) {
        const bool quality_can_relieve_memory = !result.quality.plan.actions.empty() &&
            result.quality.plan.actions.front().memory_freed_bytes != 0;
        allow_quality_execution = allow_quality_execution && quality_can_relieve_memory;
    }

    if (!config_.allow_combined_actions && result.memory.executed_actions != 0) {
        allow_quality_execution = false;
    }

    if (allow_quality_execution) execute_quality(result.quality, frame, result);

    const bool memory_work = result.memory.executed_actions != 0;
    const bool quality_work = result.quality_executed;
    if (coordinator_.requested_mode() == RuntimeMode::ObserveOnly) {
        result.path = UnifiedGovernorPath::ObserveOnly;
    } else if (coordinator_.requested_mode() == RuntimeMode::PlanOnly) {
        result.path = UnifiedGovernorPath::PlanOnly;
    } else if (quality_circuit_open_ || coordinator_.circuit_open()) {
        result.path = UnifiedGovernorPath::CircuitOpen;
    } else if (memory_work && quality_work) {
        result.path = UnifiedGovernorPath::Combined;
        ++metrics_.combined_ticks;
    } else if (quality_work) {
        result.path = result.quality.kind == QualityDecisionKind::Restore
            ? UnifiedGovernorPath::Restore
            : UnifiedGovernorPath::Quality;
    } else if (memory_work) {
        result.path = UnifiedGovernorPath::Memory;
    } else if (result.quality.kind == QualityDecisionKind::Restore) {
        result.path = UnifiedGovernorPath::Restore;
    } else {
        result.path = UnifiedGovernorPath::None;
    }

    result.quality_circuit_open = quality_circuit_open_;
    return result;
}

} // namespace arc
