#include "arc/residency.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace arc {
namespace {
std::uint64_t ratio_bytes(std::uint64_t bytes, double ratio) noexcept {
    if (!bytes || !(ratio > 0.0)) { return 0; }
    if (ratio >= 1.0) { return bytes; }
    return static_cast<std::uint64_t>(static_cast<long double>(bytes) * static_cast<long double>(ratio));
}
}
bool ResidencyGovernor::register_object(ResidencyObject object) {
    if (!object.id || objects_.contains(object.id)) { return false; }
    if (object.safety == ResidencySafety::Pinned) { object.state = ResidencyState::Pinned; }
    else if (object.safety == ResidencySafety::Unknown) { object.state = ResidencyState::Unknown; }
    return objects_.emplace(object.id, object).second;
}
bool ResidencyGovernor::note_use(ResidencyId id, std::uint64_t epoch, std::uint64_t submittedFence, std::uint64_t completedFence) {
    auto it = objects_.find(id); if (it == objects_.end()) { return false; }
    auto& object = it->second;
    if (object.use_count && epoch > object.last_use_epoch) {
        const auto interval = static_cast<double>(epoch - object.last_use_epoch);
        object.cost.reuse_interval = object.cost.reuse_interval ? .875 * object.cost.reuse_interval + .125 * interval : interval;
    }
    object.last_use_epoch = epoch;
    object.last_use_fence = submittedFence;
    object.last_completed_fence = (std::max)(object.last_completed_fence, completedFence);
    ++object.use_count;
    return true;
}
bool ResidencyGovernor::note_completed(ResidencyId id, std::uint64_t completedFence) {
    auto it = objects_.find(id); if (it == objects_.end()) { return false; }
    it->second.last_completed_fence = (std::max)(it->second.last_completed_fence, completedFence);
    return true;
}
bool ResidencyGovernor::transition(ResidencyId id, ResidencyState expected, ResidencyState next) {
    auto it = objects_.find(id); if (it == objects_.end() || it->second.state != expected) { return false; }
    if (it->second.safety != ResidencySafety::ControlledSafe || expected == ResidencyState::Pinned || expected == ResidencyState::Unknown) { return false; }
    if (next == ResidencyState::Evicted && !eviction_fence_safe(it->second)) { return false; }
    it->second.state = next;
    return true;
}
void ResidencyGovernor::update_budget(std::uint64_t budget, std::uint64_t usage) {
    budget_ = budget; usage_ = usage;
    if (!budget) { pressure_ = PressureState::Emergency; stable_samples_ = 0; return; }
    const auto ratio = static_cast<double>(usage) / static_cast<double>(budget);
    if (ratio >= config_.emergency_enter) { pressure_ = PressureState::Emergency; stable_samples_ = 0; return; }
    if (pressure_ == PressureState::Emergency) {
        if (ratio < config_.emergency_exit && ++stable_samples_ >= config_.recovery_samples) { pressure_ = PressureState::Pressure; stable_samples_ = 0; }
        else if (ratio >= config_.emergency_exit) { stable_samples_ = 0; }
        return;
    }
    if (pressure_ == PressureState::Pressure) {
        if (ratio < config_.pressure_exit && ++stable_samples_ >= config_.recovery_samples) { pressure_ = PressureState::Normal; stable_samples_ = 0; }
        else if (ratio >= config_.pressure_exit) { stable_samples_ = 0; }
        return;
    }
    if (ratio >= config_.pressure_enter) { pressure_ = PressureState::Pressure; stable_samples_ = 0; }
}
std::uint64_t ResidencyGovernor::bytes_to_free() const noexcept {
    if (!budget_ || pressure_ == PressureState::Normal) { return 0; }
    const auto ratio = pressure_ == PressureState::Emergency ? config_.emergency_target : config_.pressure_target;
    auto target = ratio_bytes(budget_, ratio);
    if (config_.minimum_headroom_bytes) {
        const auto headroomTarget = budget_ > config_.minimum_headroom_bytes ? budget_ - config_.minimum_headroom_bytes : 0;
        target = (std::min)(target, headroomTarget);
    }
    return usage_ > target ? usage_ - target : 0;
}
bool ResidencyGovernor::eviction_fence_safe(const ResidencyObject& object) const noexcept {
    return object.last_use_fence == 0 || object.last_completed_fence >= object.last_use_fence;
}
std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(const ResidencyObject& object) const {
    if (object.use_count < 2 || !(object.cost.reuse_interval > 0.0)) { return std::nullopt; }
    const auto rounded = (std::max)(1.0, std::round(object.cost.reuse_interval));
    const auto interval = rounded >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)()) ? (std::numeric_limits<std::uint64_t>::max)() : static_cast<std::uint64_t>(rounded);
    if (object.last_use_epoch > (std::numeric_limits<std::uint64_t>::max)() - interval) { return (std::numeric_limits<std::uint64_t>::max)(); }
    return object.last_use_epoch + interval;
}
std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(ResidencyId id) const {
    const auto it = objects_.find(id); return it == objects_.end() ? std::nullopt : predicted_next_use(it->second);
}
std::vector<ResidencyAction> ResidencyGovernor::plan_evictions(std::uint64_t epoch) const {
    const auto required = bytes_to_free();
    if (!required) { return {}; }
    std::vector<ResidencyAction> candidates;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Resident || !eviction_fence_safe(object)) { continue; }
        const auto age = epoch >= object.last_use_epoch ? epoch - object.last_use_epoch : 0;
        if (age < config_.minimum_residency_age_epochs) { continue; }
        const auto predicted = predicted_next_use(object);
        if (predicted && *predicted <= epoch + config_.prefetch_horizon_epochs) { continue; }
        const auto ageFactor = object.use_count && object.cost.reuse_interval > 0.0 ? static_cast<double>(age) / object.cost.reuse_interval : 4.0;
        const auto kib = static_cast<double>((std::max<std::uint64_t>)(1, object.cost.bytes / 1024));
        const auto score = ageFactor * std::sqrt(kib) / (1.0 + (std::max)(0.0, object.cost.reload_ms));
        candidates.push_back({ResidencyAction::Type::Evict, id, object.cost.bytes, predicted.value_or(0), score});
    }
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.score != right.score) { return left.score > right.score; }
        if (left.bytes != right.bytes) { return left.bytes > right.bytes; }
        return left.object < right.object;
    });
    std::vector<ResidencyAction> result;
    std::uint64_t planned{};
    for (const auto& action : candidates) {
        result.push_back(action);
        planned = action.bytes > (std::numeric_limits<std::uint64_t>::max)() - planned ? (std::numeric_limits<std::uint64_t>::max)() : planned + action.bytes;
        if (planned >= required) { break; }
    }
    return result;
}
std::vector<ResidencyAction> ResidencyGovernor::plan_promotions(std::uint64_t epoch) const {
    std::vector<ResidencyAction> result;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) { continue; }
        const auto predicted = predicted_next_use(object);
        if (!predicted) { continue; }
        if (*predicted <= epoch + config_.prefetch_horizon_epochs) {
            const auto distance = *predicted > epoch ? *predicted - epoch : 0;
            const auto score = 1.0 / (1.0 + static_cast<double>(distance));
            result.push_back({ResidencyAction::Type::MakeResident, id, object.cost.bytes, *predicted, score});
        }
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.predicted_use_epoch != right.predicted_use_epoch) { return left.predicted_use_epoch < right.predicted_use_epoch; }
        return left.object < right.object;
    });
    return result;
}
std::vector<ResidencyAction> ResidencyGovernor::plan(std::uint64_t epoch) const {
    auto actions = plan_promotions(epoch);
    auto evictions = plan_evictions(epoch);
    actions.insert(actions.end(), evictions.begin(), evictions.end());
    return actions;
}
std::optional<ResidencyObject> ResidencyGovernor::find(ResidencyId id) const {
    const auto it = objects_.find(id); return it == objects_.end() ? std::nullopt : std::optional(it->second);
}
void ResidencyGovernor::record_eviction(ResidencyId id, bool laterReloaded) {
    const auto it = objects_.find(id); if (it == objects_.end()) { return; }
    metrics_.bytes_evicted += it->second.cost.bytes;
    if (laterReloaded) { ++metrics_.false_evictions; } else { ++metrics_.useful_evictions; }
}
void ResidencyGovernor::record_resident(ResidencyId id, bool late) {
    const auto it = objects_.find(id); if (it == objects_.end()) { return; }
    ++metrics_.reloads; metrics_.bytes_made_resident += it->second.cost.bytes; metrics_.late_residency += late;
}
}
