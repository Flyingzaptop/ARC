#include "arc/live_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace arc {
namespace {

LiveRuntimeConfig normalize_config(LiveRuntimeConfig config) {
    if (!std::isfinite(config.minimum_transition_prefetch_confidence) ||
        config.minimum_transition_prefetch_confidence < 0.0 ||
        config.minimum_transition_prefetch_confidence > 1.0) {
        config.minimum_transition_prefetch_confidence = 0.40;
    }
    if (!config.max_transition_prefetch_actions) config.max_transition_prefetch_actions = 4;
    if (!config.max_restore_bytes_per_tick) config.max_restore_bytes_per_tick = 256ull * 1024ull * 1024ull;
    return config;
}

std::uint64_t ratio_bytes(std::uint64_t bytes, double ratio) noexcept {
    if (!bytes || !(ratio > 0.0)) return 0;
    if (ratio >= 1.0) return bytes;
    const auto scaled = static_cast<long double>(bytes) * static_cast<long double>(ratio);
    return static_cast<std::uint64_t>(std::floor(scaled));
}

bool nearly_equal(double left, double right) noexcept {
    const auto scale = (std::max)({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= scale * 1e-9;
}

}  // namespace

LiveRuntimeController::LiveRuntimeController(LiveRuntimeConfig config)
    : config_(normalize_config(config)), residency_(config_.residency), textures_(config_.textures),
      planner_(config_.planner), transitions_(config_.transitions) {}

bool LiveRuntimeController::register_controlled_resource(ResidencyObject object) {
    if (!object.resource || !object.id || object.safety != ResidencySafety::ControlledSafe ||
        object.state != ResidencyState::Resident || residency_by_resource_.contains(object.resource) ||
        resource_by_residency_.contains(object.id)) return false;
    if (!residency_.register_object(object)) return false;
    residency_by_resource_.emplace(object.resource, object.id);
    resource_by_residency_.emplace(object.id, object.resource);
    ++metrics_.resources_registered;
    return true;
}

bool LiveRuntimeController::register_controlled_texture(TextureQualityObject object) {
    if (!object.resource || !object.id || object.safety != TextureQualitySafety::MipSafe ||
        texture_by_resource_.contains(object.resource) || resource_by_texture_.contains(object.id)) return false;
    if (!textures_.register_texture(object)) return false;
    texture_by_resource_.emplace(object.resource, object.id);
    resource_by_texture_.emplace(object.id, object.resource);
    ++metrics_.resources_registered;
    return true;
}

bool LiveRuntimeController::unregister_resource(ResourceId resource) noexcept {
    bool removed = false;
    if (const auto residency = residency_by_resource_.find(resource); residency != residency_by_resource_.end()) {
        const auto id = residency->second;
        removed |= residency_.unregister_object(id);
        resource_by_residency_.erase(id);
        residency_by_resource_.erase(residency);
    }
    if (const auto texture = texture_by_resource_.find(resource); texture != texture_by_resource_.end()) {
        const auto id = texture->second;
        removed |= textures_.unregister_texture(id);
        resource_by_texture_.erase(id);
        texture_by_resource_.erase(texture);
    }
    inflight_counts_.erase(resource);
    if (removed) ++metrics_.resources_unregistered;
    return removed;
}

bool LiveRuntimeController::note_use(
    ResourceId resource, std::uint64_t epoch, QueueId queue,
    std::uint64_t submitted_fence, std::uint64_t completed_fence) {
    if (!resource) return false;
    transitions_.observe(resource);
    ++metrics_.observed_uses;
    const auto binding = residency_by_resource_.find(resource);
    if (binding == residency_by_resource_.end()) return true;
    ++metrics_.controlled_uses;
    if (!queue || !submitted_fence || !residency_.note_use(binding->second, epoch, queue, submitted_fence, completed_fence)) {
        ++metrics_.rejected_controlled_uses;
        return false;
    }
    return true;
}

bool LiveRuntimeController::note_completed(ResourceId resource, std::uint64_t completed_fence) {
    const auto binding = residency_by_resource_.find(resource);
    return binding != residency_by_resource_.end() && residency_.note_completed(binding->second, completed_fence);
}

bool LiveRuntimeController::mark_inflight(ResourceId resource) noexcept {
    if (!controlled(resource)) return true;
    auto& count = inflight_counts_[resource];
    if (count == (std::numeric_limits<std::uint32_t>::max)()) return false;
    ++count;
    ++metrics_.inflight_blocks_started;
    return true;
}

bool LiveRuntimeController::release_inflight(ResourceId resource, std::uint64_t completed_fence) noexcept {
    if (!controlled(resource)) return true;
    const auto it = inflight_counts_.find(resource);
    if (it == inflight_counts_.end() || it->second == 0) return false;
    if (const auto residency = residency_by_resource_.find(resource); residency != residency_by_resource_.end()) {
        if (!residency_.note_completed(residency->second, completed_fence)) return false;
    }
    if (--it->second == 0) inflight_counts_.erase(it);
    ++metrics_.inflight_blocks_completed;
    return true;
}

std::uint32_t LiveRuntimeController::inflight_count(ResourceId resource) const noexcept {
    const auto it = inflight_counts_.find(resource);
    return it == inflight_counts_.end() ? 0U : it->second;
}

bool LiveRuntimeController::externally_inflight(ResourceId resource) const noexcept {
    return inflight_count(resource) != 0;
}

void LiveRuntimeController::update_budget(const MemoryBudgetPayload& budget) {
    residency_.update_budget(budget.local_budget, budget.local_usage);
}

std::uint64_t LiveRuntimeController::restore_headroom() const noexcept {
    if (residency_.pressure() != PressureState::Normal || !residency_.budget()) return 0;
    const auto ceiling = ratio_bytes(residency_.budget(), residency_.config().promotion_ceiling);
    if (residency_.usage() >= ceiling) return 0;
    auto headroom = ceiling - residency_.usage();
    if (headroom <= config_.minimum_restore_reserve_bytes) return 0;
    headroom -= config_.minimum_restore_reserve_bytes;
    return (std::min)(headroom, config_.max_restore_bytes_per_tick);
}

std::vector<ResidencyAction> LiveRuntimeController::transition_prefetch(
    std::uint64_t epoch, std::uint64_t headroom) {
    std::vector<ResidencyAction> result;
    if (!headroom || residency_.pressure() != PressureState::Normal) return result;
    const auto minimum_confidence = (std::max)(config_.minimum_transition_prefetch_confidence, transitions_.config().minimum_confidence);
    for (const auto& prediction : transitions_.predictions()) {
        if (result.size() >= config_.max_transition_prefetch_actions) break;
        if (prediction.confidence < minimum_confidence || externally_inflight(prediction.resource)) continue;
        const auto binding = residency_by_resource_.find(prediction.resource);
        if (binding == residency_by_resource_.end()) continue;
        const auto object = residency_.find(binding->second);
        if (!object || object->state != ResidencyState::Evicted || object->safety != ResidencySafety::ControlledSafe) continue;
        const auto action = residency_.require_resident(binding->second);
        if (!action || action->bytes > headroom) continue;
        auto predicted_action = *action;
        predicted_action.predicted_use_epoch = epoch == (std::numeric_limits<std::uint64_t>::max)() ? epoch : epoch + 1;
        predicted_action.score = prediction.confidence * (1.0 + (std::max)(0.0, object->cost.reload_ms));
        result.push_back(predicted_action);
        headroom -= predicted_action.bytes;
    }
    return result;
}

LiveRuntimePlan LiveRuntimeController::plan(std::uint64_t epoch) {
    LiveRuntimePlan result{};
    result.epoch = epoch;
    result.pressure = residency_.pressure();
    result.local_budget = residency_.budget();
    result.local_usage = residency_.usage();
    if (result.pressure != PressureState::Normal) {
        result.pressure_relief = planner_.plan_pressure_relief(residency_, textures_, epoch);
        if (result.pressure_relief.requested_bytes) ++metrics_.pressure_plans;
        return result;
    }
    result.restore_headroom = restore_headroom();
    result.transition_prefetch = transition_prefetch(epoch, result.restore_headroom);
    if (!result.transition_prefetch.empty()) {
        ++metrics_.transition_prefetch_plans;
        metrics_.transition_prefetch_actions += result.transition_prefetch.size();
        return result;
    }
    if (result.restore_headroom) {
        result.restore = planner_.plan_headroom_restore(residency_, textures_, epoch, result.restore_headroom);
        if (!result.restore.arbitration.actions.empty()) ++metrics_.restore_plans;
    }
    return result;
}

std::optional<std::vector<LiveRuntimeResolvedAction>> LiveRuntimeController::resolve_pressure_actions(
    const GlobalMemoryPlan& plan_snapshot, std::uint64_t epoch) const {
    std::vector<LiveRuntimeResolvedAction> result;
    result.reserve(plan_snapshot.arbitration.actions.size());
    const auto residency_candidates = residency_.eviction_candidates(epoch);
    const auto texture_candidates = textures_.demotion_candidates(epoch);
    for (const auto& planned : plan_snapshot.arbitration.actions) {
        const auto& candidate = planned.candidate;
        if (externally_inflight(candidate.resource)) return std::nullopt;
        if (candidate.kind == MemoryActionKind::EvictResource) {
            if (candidate.sequence != 0) return std::nullopt;
            const auto binding = resource_by_residency_.find(candidate.subject);
            if (binding == resource_by_residency_.end() || binding->second != candidate.resource) return std::nullopt;
            const auto found = std::find_if(residency_candidates.begin(), residency_candidates.end(), [&](const auto& action) {
                return action.object == candidate.subject && action.bytes == candidate.bytes_freed;
            });
            if (found == residency_candidates.end()) return std::nullopt;
            result.emplace_back(*found);
            continue;
        }
        if (candidate.kind == MemoryActionKind::DemoteTexture) {
            const auto binding = resource_by_texture_.find(candidate.subject);
            if (binding == resource_by_texture_.end() || binding->second != candidate.resource) return std::nullopt;
            std::uint32_t sequence{};
            const TextureQualityAction* selected{};
            for (const auto& action : texture_candidates) {
                if (action.texture != candidate.subject) continue;
                if (sequence++ == candidate.sequence) { selected = &action; break; }
            }
            if (!selected || selected->resource != candidate.resource || selected->bytes_delta != candidate.bytes_freed ||
                !nearly_equal(selected->quality_delta < 0.0 ? -selected->quality_delta : 0.0, candidate.quality_loss)) return std::nullopt;
            result.emplace_back(*selected);
            continue;
        }
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<LiveRuntimeResolvedAction>> LiveRuntimeController::resolve_restore_actions(
    const GlobalMemoryRestorePlan& plan_snapshot, std::uint64_t epoch) const {
    std::vector<LiveRuntimeResolvedAction> result;
    result.reserve(plan_snapshot.arbitration.actions.size());
    const auto residency_candidates = residency_.promotion_candidates(epoch);
    const auto texture_candidates = textures_.promotion_candidates(epoch);
    for (const auto& planned : plan_snapshot.arbitration.actions) {
        const auto& candidate = planned.candidate;
        if (externally_inflight(candidate.resource)) return std::nullopt;
        if (candidate.kind == MemoryRestoreKind::MakeResident) {
            if (candidate.sequence != 0) return std::nullopt;
            const auto binding = resource_by_residency_.find(candidate.subject);
            if (binding == resource_by_residency_.end() || binding->second != candidate.resource) return std::nullopt;
            const auto found = std::find_if(residency_candidates.begin(), residency_candidates.end(), [&](const auto& action) {
                return action.object == candidate.subject && action.bytes == candidate.bytes_cost;
            });
            if (found == residency_candidates.end()) return std::nullopt;
            result.emplace_back(*found);
            continue;
        }
        if (candidate.kind == MemoryRestoreKind::PromoteTexture) {
            const auto binding = resource_by_texture_.find(candidate.subject);
            if (binding == resource_by_texture_.end() || binding->second != candidate.resource) return std::nullopt;
            std::uint32_t sequence{};
            const TextureQualityAction* selected{};
            for (const auto& action : texture_candidates) {
                if (action.texture != candidate.subject) continue;
                if (sequence++ == candidate.sequence) { selected = &action; break; }
            }
            if (!selected || selected->resource != candidate.resource || selected->bytes_delta != candidate.bytes_cost ||
                !nearly_equal(selected->quality_delta > 0.0 ? selected->quality_delta : 0.0, candidate.quality_gain)) return std::nullopt;
            result.emplace_back(*selected);
            continue;
        }
        return std::nullopt;
    }
    return result;
}

bool LiveRuntimeController::begin_residency_action(const ResidencyAction& action, std::uint64_t epoch, bool demand_miss) {
    const auto binding = resource_by_residency_.find(action.object);
    if (binding == resource_by_residency_.end() || externally_inflight(binding->second)) return false;
    switch (action.type) {
    case ResidencyAction::Type::Evict:
        if (!residency_.transition(action.object, ResidencyState::Resident, ResidencyState::Evicted)) return false;
        residency_.record_eviction(action.object, false, epoch);
        return true;
    case ResidencyAction::Type::MakeResident:
        if (!residency_.transition(action.object, ResidencyState::Evicted, ResidencyState::PendingResident)) return false;
        residency_.record_resident(action.object, demand_miss, epoch);
        return true;
    }
    return false;
}

bool LiveRuntimeController::complete_make_resident(ResidencyId object) {
    return resource_by_residency_.contains(object) && residency_.transition(object, ResidencyState::PendingResident, ResidencyState::Resident);
}

bool LiveRuntimeController::apply_texture_action(const TextureQualityAction& action, std::uint64_t epoch) {
    const auto resource = resource_by_texture_.find(action.texture);
    return resource != resource_by_texture_.end() && resource->second == action.resource &&
        !externally_inflight(action.resource) && textures_.apply(action, epoch);
}

bool LiveRuntimeController::controlled(ResourceId resource) const noexcept {
    return residency_by_resource_.contains(resource) || texture_by_resource_.contains(resource);
}

}  // namespace arc
