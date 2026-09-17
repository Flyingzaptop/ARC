#include "arc/runtime_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace arc {

RuntimeCoordinator::RuntimeCoordinator(
    LiveRuntimeController& runtime,
    RuntimeMutationBackend* backend,
    RuntimeCoordinatorConfig config)
    : runtime_(runtime), backend_(backend), config_(config), requested_mode_(config.mode) {
    if (!config_.max_consecutive_failures) config_.max_consecutive_failures = 1;
    if (!config_.max_actions_per_tick) config_.max_actions_per_tick = 1;
}

RuntimeMode RuntimeCoordinator::effective_mode() const noexcept {
    if (circuit_open_) return RuntimeMode::ObserveOnly;
    if (requested_mode_ == RuntimeMode::Controlled && backend_ == nullptr) return RuntimeMode::PlanOnly;
    return requested_mode_;
}

void RuntimeCoordinator::reset_circuit_breaker() noexcept {
    circuit_open_ = false;
    consecutive_failures_ = 0;
}

void RuntimeCoordinator::trip_circuit() noexcept {
    if (!circuit_open_) ++metrics_.circuit_trips;
    circuit_open_ = true;
}

void RuntimeCoordinator::record_failure(bool immediate_trip) noexcept {
    if (consecutive_failures_ != (std::numeric_limits<std::uint32_t>::max)()) ++consecutive_failures_;
    if (immediate_trip || consecutive_failures_ >= config_.max_consecutive_failures) trip_circuit();
}

bool RuntimeCoordinator::refresh_budget_freshness(RuntimeTickResult& result) noexcept {
    const auto revision = runtime_.budget_revision();
    if (revision != seen_budget_revision_) {
        seen_budget_revision_ = revision;
        if (revision != 0) {
            budget_seen_ = true;
            last_budget_tick_ = metrics_.ticks;
        }
    }

    if (!budget_seen_ || metrics_.ticks < last_budget_tick_) {
        result.budget_age_ticks = (std::numeric_limits<std::uint64_t>::max)();
        result.budget_fresh = false;
        return false;
    }

    result.budget_age_ticks = metrics_.ticks - last_budget_tick_;
    result.budget_fresh = config_.max_budget_age_ticks == 0 || result.budget_age_ticks <= config_.max_budget_age_ticks;
    return result.budget_fresh;
}

std::optional<std::vector<LiveRuntimeResolvedAction>> RuntimeCoordinator::resolve(const LiveRuntimePlan& plan) const {
    if (plan.pressure != PressureState::Normal) {
        return runtime_.resolve_pressure_actions(plan.pressure_relief, plan.epoch);
    }

    if (!plan.transition_prefetch.empty()) {
        std::vector<LiveRuntimeResolvedAction> actions;
        actions.reserve(plan.transition_prefetch.size());
        for (const auto& action : plan.transition_prefetch) actions.emplace_back(action);
        return actions;
    }

    if (!plan.restore.arbitration.actions.empty()) {
        return runtime_.resolve_restore_actions(plan.restore, plan.epoch);
    }

    return std::vector<LiveRuntimeResolvedAction>{};
}

bool RuntimeCoordinator::execute_action(
    const LiveRuntimeResolvedAction& action,
    std::uint64_t epoch,
    RuntimeTickResult& result) {
    if (!backend_) {
        result.status = RuntimeTickStatus::BackendFailed;
        result.backend_status = RuntimeBackendStatus::Unsupported;
        ++metrics_.backend_failures;
        record_failure(false);
        return false;
    }

    return std::visit([&](const auto& typed) -> bool {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, ResidencyAction>) {
            const auto resource = runtime_.residency_resource(typed.object);
            if (!resource) {
                result.status = RuntimeTickStatus::CommitFailed;
                ++metrics_.commit_failures;
                record_failure(true);
                return false;
            }

            RuntimeBackendStatus backend_status{};
            if (typed.type == ResidencyAction::Type::Evict) {
                backend_status = backend_->evict(*resource, typed);
            } else {
                backend_status = backend_->make_resident(*resource, typed);
            }
            result.backend_status = backend_status;
            if (backend_status != RuntimeBackendStatus::Success) {
                result.status = RuntimeTickStatus::BackendFailed;
                ++metrics_.backend_failures;
                record_failure(false);
                return false;
            }

            if (typed.type == ResidencyAction::Type::Evict) {
                if (!runtime_.begin_residency_action(typed, epoch)) {
                    ResidencyAction rollback = typed;
                    rollback.type = ResidencyAction::Type::MakeResident;
                    ++metrics_.rollback_attempts;
                    if (backend_->make_resident(*resource, rollback) != RuntimeBackendStatus::Success) {
                        ++metrics_.rollback_failures;
                    }
                    result.status = RuntimeTickStatus::CommitFailed;
                    ++metrics_.commit_failures;
                    record_failure(true);
                    return false;
                }
                return true;
            }

            if (!runtime_.begin_residency_action(typed, epoch, false) ||
                !runtime_.complete_make_resident(typed.object)) {
                result.status = RuntimeTickStatus::CommitFailed;
                ++metrics_.commit_failures;
                record_failure(true);
                return false;
            }
            return true;
        } else {
            const auto resource = runtime_.texture_resource(typed.texture);
            if (!resource || *resource != typed.resource) {
                result.status = RuntimeTickStatus::CommitFailed;
                ++metrics_.commit_failures;
                record_failure(true);
                return false;
            }

            const auto backend_status = typed.type == TextureQualityAction::Type::Demote
                ? backend_->demote_texture(*resource, typed)
                : backend_->promote_texture(*resource, typed);
            result.backend_status = backend_status;
            if (backend_status != RuntimeBackendStatus::Success) {
                result.status = RuntimeTickStatus::BackendFailed;
                ++metrics_.backend_failures;
                record_failure(false);
                return false;
            }

            if (!runtime_.apply_texture_action(typed, epoch)) {
                if (typed.type == TextureQualityAction::Type::Demote) {
                    TextureQualityAction rollback = typed;
                    rollback.type = TextureQualityAction::Type::Promote;
                    rollback.from_level = typed.to_level;
                    rollback.to_level = typed.from_level;
                    rollback.quality_delta = -typed.quality_delta;
                    ++metrics_.rollback_attempts;
                    if (backend_->promote_texture(*resource, rollback) != RuntimeBackendStatus::Success) {
                        ++metrics_.rollback_failures;
                    }
                }
                result.status = RuntimeTickStatus::CommitFailed;
                ++metrics_.commit_failures;
                record_failure(true);
                return false;
            }
            return true;
        }
    }, action);
}

RuntimeTickResult RuntimeCoordinator::execute_plan(const LiveRuntimePlan& plan) {
    RuntimeTickResult result{};
    ++metrics_.ticks;
    result.requested_mode = requested_mode_;
    result.effective_mode = effective_mode();
    result.circuit_open = circuit_open_;
    (void)refresh_budget_freshness(result);
    result.plan = plan;

    if (circuit_open_) {
        ++metrics_.observe_only_ticks;
        result.status = RuntimeTickStatus::CircuitOpen;
        return result;
    }

    if (result.effective_mode == RuntimeMode::ObserveOnly) {
        ++metrics_.observe_only_ticks;
        result.status = RuntimeTickStatus::ObservedOnly;
        return result;
    }

    if (result.effective_mode == RuntimeMode::Controlled && !result.budget_fresh) {
        ++metrics_.controlled_ticks;
        ++metrics_.stale_budget_blocks;
        result.status = RuntimeTickStatus::BudgetStale;
        return result;
    }

    const auto resolved = resolve(result.plan);
    if (!resolved) {
        ++metrics_.resolve_failures;
        runtime_.record_resolve_failure();
        result.status = RuntimeTickStatus::ResolveFailed;
        if (result.effective_mode == RuntimeMode::Controlled) record_failure(false);
        return result;
    }

    result.resolved_actions = resolved->size();
    metrics_.resolved_actions += resolved->size();
    if (resolved->empty()) {
        if (result.effective_mode == RuntimeMode::PlanOnly) ++metrics_.plan_only_ticks;
        else ++metrics_.controlled_ticks;
        result.status = RuntimeTickStatus::NoAction;
        return result;
    }

    if (result.effective_mode == RuntimeMode::PlanOnly) {
        ++metrics_.plan_only_ticks;
        result.status = RuntimeTickStatus::PlannedOnly;
        return result;
    }

    ++metrics_.controlled_ticks;
    const auto action_limit = (std::min<std::size_t>)(resolved->size(), config_.max_actions_per_tick);
    for (std::size_t index = 0; index < action_limit; ++index) {
        if (!execute_action((*resolved)[index], plan.epoch, result)) {
            result.circuit_open = circuit_open_;
            result.effective_mode = effective_mode();
            return result;
        }
        ++result.executed_actions;
        ++metrics_.executed_actions;
    }

    consecutive_failures_ = 0;
    result.status = result.executed_actions ? RuntimeTickStatus::Executed : RuntimeTickStatus::NoAction;
    result.circuit_open = circuit_open_;
    result.effective_mode = effective_mode();
    return result;
}

RuntimeTickResult RuntimeCoordinator::tick(std::uint64_t epoch) {
    return execute_plan(runtime_.plan(epoch));
}

RuntimeTickResult RuntimeCoordinator::tick_with_plan(const LiveRuntimePlan& plan) {
    return execute_plan(plan);
}

}  // namespace arc
