#include "arc/residency.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace arc {
namespace {
std::uint64_t ratio_bytes(std::uint64_t bytes, double ratio) noexcept {
    if (!bytes || !(ratio > 0.0)) return 0;
    if (ratio >= 1.0) return bytes;
    const auto scaled = static_cast<long double>(bytes) * static_cast<long double>(ratio);
    return static_cast<std::uint64_t>(std::floor(scaled + 0.5L));
}
bool finite_ratio(double v) noexcept { return std::isfinite(v) && v >= 0.0 && v <= 1.0; }
bool valid_config(const ResidencyPolicyConfig& c) noexcept {
    return finite_ratio(c.pressure_enter) && finite_ratio(c.emergency_enter) && finite_ratio(c.pressure_exit) &&
        finite_ratio(c.emergency_exit) && finite_ratio(c.pressure_target) && finite_ratio(c.emergency_target) &&
        finite_ratio(c.promotion_ceiling) && c.recovery_samples > 0 && c.pressure_target < c.pressure_enter &&
        c.pressure_exit < c.pressure_enter && c.emergency_target < c.emergency_enter &&
        c.emergency_exit < c.emergency_enter && c.pressure_enter < c.emergency_enter && c.promotion_ceiling <= c.pressure_enter;
}
}
ResidencyGovernor::ResidencyGovernor(ResidencyPolicyConfig config) : config_(valid_config(config) ? config : ResidencyPolicyConfig{}) {}
bool ResidencyGovernor::register_object(ResidencyObject object) {
    if (!object.id || !object.cost.bytes || objects_.contains(object.id)) return false;
    if (object.safety == ResidencySafety::Pinned) object.state = ResidencyState::Pinned;
    else if (object.safety == ResidencySafety::Unknown) object.state = ResidencyState::Unknown;
    return objects_.emplace(object.id, object).second;
}
bool ResidencyGovernor::note_use(ResidencyId id, std::uint64_t epoch, QueueId queue, std::uint64_t submittedFence, std::uint64_t completedFence) {
    auto it = objects_.find(id); if (it == objects_.end()) return false;
    auto& object = it->second;
    if (object.use_count && epoch > object.last_use_epoch) {
        const auto interval = static_cast<double>(epoch - object.last_use_epoch);
        object.cost.reuse_interval = object.cost.reuse_interval ? .875 * object.cost.reuse_interval + .125 * interval : interval;
    }
    object.last_use_epoch = epoch; object.last_use_queue = queue; object.last_use_fence = submittedFence;
    object.last_completed_fence = (std::max)(object.last_completed_fence, completedFence); ++object.use_count; return true;
}
bool ResidencyGovernor::note_completed(ResidencyId id, std::uint64_t completedFence) {
    auto it = objects_.find(id); if (it == objects_.end()) return false;
    it->second.last_completed_fence = (std::max)(it->second.last_completed_fence, completedFence); return true;
}
bool ResidencyGovernor::transition(ResidencyId id, ResidencyState expected, ResidencyState next) {
    auto it = objects_.find(id); if (it == objects_.end() || it->second.state != expected) return false;
    if (it->second.safety != ResidencySafety::ControlledSafe || expected == ResidencyState::Pinned || expected == ResidencyState::Unknown) return false;
    const bool legal = (expected == ResidencyState::Resident && next == ResidencyState::Evicted) ||
        (expected == ResidencyState::Evicted && next == ResidencyState::PendingResident) ||
        (expected == ResidencyState::PendingResident && next == ResidencyState::Resident);
    if (!legal) return false;
    if (next == ResidencyState::Evicted && !eviction_fence_safe(it->second)) return false;
    it->second.state = next; return true;
}
void ResidencyGovernor::update_budget(std::uint64_t budget, std::uint64_t usage) {
    budget_ = budget; usage_ = usage;
    if (!budget) { pressure_ = PressureState::Emergency; stable_samples_ = 0; return; }
    const auto ratio = static_cast<double>(usage) / static_cast<double>(budget);
    if (ratio >= config_.emergency_enter) { pressure_ = PressureState::Emergency; stable_samples_ = 0; return; }
    if (pressure_ == PressureState::Emergency) {
        if (ratio < config_.emergency_exit && ++stable_samples_ >= config_.recovery_samples) { pressure_ = PressureState::Pressure; stable_samples_ = 0; }
        else if (ratio >= config_.emergency_exit) stable_samples_ = 0;
        return;
    }
    if (pressure_ == PressureState::Pressure) {
        if (ratio < config_.pressure_exit && ++stable_samples_ >= config_.recovery_samples) { pressure_ = PressureState::Normal; stable_samples_ = 0; }
        else if (ratio >= config_.pressure_exit) stable_samples_ = 0;
        return;
    }
    if (ratio >= config_.pressure_enter) { pressure_ = PressureState::Pressure; stable_samples_ = 0; }
}
std::uint64_t ResidencyGovernor::target_usage_bytes() const noexcept {
    if (!budget_) return 0;
    if (pressure_ == PressureState::Normal) return usage_;
    const auto ratio = pressure_ == PressureState::Emergency ? config_.emergency_target : config_.pressure_target;
    auto target = ratio_bytes(budget_, ratio);
    if (config_.minimum_headroom_bytes) {
        const auto headroomTarget = budget_ > config_.minimum_headroom_bytes ? budget_ - config_.minimum_headroom_bytes : 0;
        target = (std::min)(target, headroomTarget);
    }
    return target;
}
std::uint64_t ResidencyGovernor::bytes_to_free() const noexcept {
    const auto target = target_usage_bytes();
    return pressure_ == PressureState::Normal || usage_ <= target ? 0 : usage_ - target;
}
bool ResidencyGovernor::eviction_fence_safe(const ResidencyObject& object) const noexcept {
    return object.last_use_fence == 0 || object.last_completed_fence >= object.last_use_fence;
}
std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(const ResidencyObject& object, std::uint64_t epoch) const {
    if (object.use_count < 2 || !(object.cost.reuse_interval > 0.0) || !std::isfinite(object.cost.reuse_interval)) return std::nullopt;
    const auto rounded = (std::max)(1.0, std::round(object.cost.reuse_interval));
    const auto maxU64 = (std::numeric_limits<std::uint64_t>::max)();
    const auto interval = rounded >= static_cast<double>(maxU64) ? maxU64 : static_cast<std::uint64_t>(rounded);
    if (!interval || object.last_use_epoch > maxU64 - interval) return maxU64;
    auto next = object.last_use_epoch + interval;
    if (next <= epoch && interval != maxU64) {
        const auto elapsed = epoch - object.last_use_epoch;
        const auto cycles = elapsed / interval + 1;
        if (cycles > (maxU64 - object.last_use_epoch) / interval) return maxU64;
        next = object.last_use_epoch + cycles * interval;
    }
    return next;
}
std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(ResidencyId id, std::uint64_t epoch) const {
    const auto it = objects_.find(id); return it == objects_.end() ? std::nullopt : predicted_next_use(it->second, epoch);
}
std::vector<ResidencyAction> ResidencyGovernor::plan_evictions(std::uint64_t epoch) const {
    const auto required = bytes_to_free(); if (!required) return {};
    std::vector<ResidencyAction> candidates;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Resident || !eviction_fence_safe(object)) continue;
        const auto age = epoch >= object.last_use_epoch ? epoch - object.last_use_epoch : 0;
        if (age < config_.minimum_residency_age_epochs) continue;
        const auto predicted = predicted_next_use(object, epoch);
        if (predicted && *predicted <= epoch + config_.prefetch_horizon_epochs) continue;
        const auto ageFactor = object.use_count && object.cost.reuse_interval > 0.0 ? static_cast<double>(age) / object.cost.reuse_interval : 4.0;
        const auto kib = static_cast<double>((std::max<std::uint64_t>)(1, object.cost.bytes / 1024));
        const auto score = ageFactor * std::sqrt(kib) / (1.0 + (std::max)(0.0, object.cost.reload_ms));
        candidates.push_back({ResidencyAction::Type::Evict,id,object.cost.bytes,predicted.value_or(0),score,object.last_use_queue,object.last_use_fence});
    }
    std::ranges::sort(candidates, [](const auto& l, const auto& r) {
        if (l.score != r.score) return l.score > r.score;
        if (l.bytes != r.bytes) return l.bytes > r.bytes;
        return l.object < r.object;
    });
    std::vector<ResidencyAction> result; std::uint64_t planned{};
    for (const auto& action : candidates) {
        result.push_back(action);
        planned = action.bytes > (std::numeric_limits<std::uint64_t>::max)() - planned ? (std::numeric_limits<std::uint64_t>::max)() : planned + action.bytes;
        if (planned >= required) break;
    }
    return result;
}
std::vector<ResidencyAction> ResidencyGovernor::plan_promotions(std::uint64_t epoch) const {
    if (!speculative_promotions_allowed() || !budget_) return {};
    std::vector<ResidencyAction> candidates;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) continue;
        const auto predicted = predicted_next_use(object, epoch);
        if (!predicted || *predicted > epoch + config_.prefetch_horizon_epochs) continue;
        const auto distance = *predicted > epoch ? *predicted - epoch : 0;
        const auto score = (1.0 + (std::max)(0.0, object.cost.reload_ms)) / (1.0 + static_cast<double>(distance));
        candidates.push_back({ResidencyAction::Type::MakeResident,id,object.cost.bytes,*predicted,score});
    }
    std::ranges::sort(candidates, [](const auto& l, const auto& r) {
        if (l.predicted_use_epoch != r.predicted_use_epoch) return l.predicted_use_epoch < r.predicted_use_epoch;
        if (l.score != r.score) return l.score > r.score;
        return l.object < r.object;
    });
    const auto ceiling = ratio_bytes(budget_, config_.promotion_ceiling);
    const auto headroomCeiling = config_.minimum_headroom_bytes && budget_ > config_.minimum_headroom_bytes ? budget_ - config_.minimum_headroom_bytes : ceiling;
    const auto target = (std::min)(ceiling, headroomCeiling);
    std::uint64_t projected = usage_; std::vector<ResidencyAction> result;
    for (const auto& action : candidates) {
        if (action.bytes > target || projected > target - action.bytes) continue;
        result.push_back(action); projected += action.bytes;
    }
    return result;
}
std::optional<ResidencyAction> ResidencyGovernor::require_resident(ResidencyId id) const {
    const auto it = objects_.find(id); if (it == objects_.end()) return std::nullopt;
    const auto& object = it->second;
    if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) return std::nullopt;
    return ResidencyAction{ResidencyAction::Type::MakeResident,id,object.cost.bytes,0,1.0};
}
std::vector<ResidencyAction> ResidencyGovernor::plan(std::uint64_t epoch) const {
    return pressure_ == PressureState::Normal ? plan_promotions(epoch) : plan_evictions(epoch);
}
ResidencyPlanSummary ResidencyGovernor::plan_summary(std::uint64_t epoch) const {
    ResidencyPlanSummary s{.pressure=pressure_,.budget=budget_,.usage=usage_,.target_usage=target_usage_bytes(),.bytes_to_free=bytes_to_free()};
    for (const auto& a : plan_evictions(epoch)) s.bytes_planned_to_free += a.bytes;
    for (const auto& a : plan_promotions(epoch)) s.bytes_planned_to_promote += a.bytes;
    s.shortfall = s.bytes_planned_to_free < s.bytes_to_free; return s;
}
std::optional<ResidencyObject> ResidencyGovernor::find(ResidencyId id) const { const auto it=objects_.find(id); return it==objects_.end()?std::nullopt:std::optional(it->second); }
void ResidencyGovernor::record_eviction(ResidencyId id, bool laterReloaded) { const auto it=objects_.find(id); if(it==objects_.end())return; metrics_.bytes_evicted+=it->second.cost.bytes; if(laterReloaded)++metrics_.false_evictions;else ++metrics_.useful_evictions; }
void ResidencyGovernor::record_resident(ResidencyId id, bool late) { const auto it=objects_.find(id); if(it==objects_.end())return; ++metrics_.reloads; metrics_.bytes_made_resident+=it->second.cost.bytes; metrics_.late_residency+=late; }
}
