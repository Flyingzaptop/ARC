#include "arc/residency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace arc {
namespace {

std::uint64_t ratio_bytes(std::uint64_t bytes, double ratio) noexcept {
    if (!bytes || !(ratio > 0.0)) {
        return 0;
    }
    if (ratio >= 1.0) {
        return bytes;
    }
    const auto scaled = static_cast<long double>(bytes) * static_cast<long double>(ratio);
    return static_cast<std::uint64_t>(std::floor(scaled + 0.5L));
}

bool finite_ratio(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool valid_config(const ResidencyPolicyConfig& config) noexcept {
    return finite_ratio(config.pressure_enter) && finite_ratio(config.emergency_enter) &&
        finite_ratio(config.pressure_exit) && finite_ratio(config.emergency_exit) &&
        finite_ratio(config.pressure_target) && finite_ratio(config.emergency_target) &&
        finite_ratio(config.promotion_ceiling) && finite_ratio(config.minimum_prefetch_confidence) &&
        config.recovery_samples > 0 && config.minimum_prediction_samples >= 2 &&
        config.prediction_stale_intervals > 0 &&
        config.pressure_target < config.pressure_enter && config.pressure_exit < config.pressure_enter &&
        config.emergency_target < config.emergency_enter && config.emergency_exit < config.emergency_enter &&
        config.pressure_enter < config.emergency_enter && config.promotion_ceiling <= config.pressure_enter;
}

}  // namespace

ResidencyGovernor::ResidencyGovernor(ResidencyPolicyConfig config)
    : config_(valid_config(config) ? config : ResidencyPolicyConfig{}) {}

bool ResidencyGovernor::register_object(ResidencyObject object) {
    if (!object.id || !object.cost.bytes || objects_.contains(object.id)) {
        return false;
    }
    if (object.safety == ResidencySafety::Pinned) {
        object.state = ResidencyState::Pinned;
    } else if (object.safety == ResidencySafety::Unknown) {
        object.state = ResidencyState::Unknown;
    }
    return objects_.emplace(object.id, object).second;
}

bool ResidencyGovernor::note_use(
    ResidencyId id,
    std::uint64_t epoch,
    QueueId queue,
    std::uint64_t submitted_fence,
    std::uint64_t completed_fence) {
    auto it = objects_.find(id);
    if (it == objects_.end()) {
        return false;
    }

    auto& object = it->second;
    if (object.use_count && epoch > object.last_use_epoch) {
        const auto interval = static_cast<double>(epoch - object.last_use_epoch);
        if (object.cost.reuse_interval > 0.0) {
            const auto error = std::abs(interval - object.cost.reuse_interval);
            object.cost.reuse_deviation = object.cost.reuse_deviation > 0.0
                ? 0.875 * object.cost.reuse_deviation + 0.125 * error
                : error;
            object.cost.reuse_interval = 0.875 * object.cost.reuse_interval + 0.125 * interval;
        } else {
            object.cost.reuse_interval = interval;
            object.cost.reuse_deviation = 0.0;
        }
    }

    object.last_use_epoch = epoch;
    object.last_use_queue = queue;
    object.last_use_fence = submitted_fence;
    object.last_completed_fence = (std::max)(object.last_completed_fence, completed_fence);
    ++object.use_count;
    return true;
}

bool ResidencyGovernor::note_completed(ResidencyId id, std::uint64_t completed_fence) {
    auto it = objects_.find(id);
    if (it == objects_.end()) {
        return false;
    }
    it->second.last_completed_fence = (std::max)(it->second.last_completed_fence, completed_fence);
    return true;
}

bool ResidencyGovernor::transition(ResidencyId id, ResidencyState expected, ResidencyState next) {
    auto it = objects_.find(id);
    if (it == objects_.end() || it->second.state != expected) {
        return false;
    }
    if (it->second.safety != ResidencySafety::ControlledSafe ||
        expected == ResidencyState::Pinned || expected == ResidencyState::Unknown) {
        return false;
    }

    const bool legal = (expected == ResidencyState::Resident && next == ResidencyState::Evicted) ||
        (expected == ResidencyState::Evicted && next == ResidencyState::PendingResident) ||
        (expected == ResidencyState::PendingResident && next == ResidencyState::Resident);
    if (!legal) {
        return false;
    }
    if (next == ResidencyState::Evicted && !eviction_fence_safe(it->second)) {
        return false;
    }

    it->second.state = next;
    return true;
}

void ResidencyGovernor::update_budget(std::uint64_t budget, std::uint64_t usage) {
    budget_ = budget;
    usage_ = usage;
    if (!budget) {
        pressure_ = PressureState::Emergency;
        stable_samples_ = 0;
        return;
    }

    const auto ratio = static_cast<double>(usage) / static_cast<double>(budget);
    if (ratio >= config_.emergency_enter) {
        pressure_ = PressureState::Emergency;
        stable_samples_ = 0;
        return;
    }

    if (pressure_ == PressureState::Emergency) {
        if (ratio < config_.emergency_exit && ++stable_samples_ >= config_.recovery_samples) {
            pressure_ = PressureState::Pressure;
            stable_samples_ = 0;
        } else if (ratio >= config_.emergency_exit) {
            stable_samples_ = 0;
        }
        return;
    }

    if (pressure_ == PressureState::Pressure) {
        if (ratio < config_.pressure_exit && ++stable_samples_ >= config_.recovery_samples) {
            pressure_ = PressureState::Normal;
            stable_samples_ = 0;
        } else if (ratio >= config_.pressure_exit) {
            stable_samples_ = 0;
        }
        return;
    }

    if (ratio >= config_.pressure_enter) {
        pressure_ = PressureState::Pressure;
        stable_samples_ = 0;
    }
}

std::uint64_t ResidencyGovernor::target_usage_bytes() const noexcept {
    if (!budget_) {
        return 0;
    }
    if (pressure_ == PressureState::Normal) {
        return usage_;
    }

    const auto ratio = pressure_ == PressureState::Emergency ? config_.emergency_target : config_.pressure_target;
    auto target = ratio_bytes(budget_, ratio);
    if (config_.minimum_headroom_bytes) {
        const auto headroom_target = budget_ > config_.minimum_headroom_bytes
            ? budget_ - config_.minimum_headroom_bytes
            : 0;
        target = (std::min)(target, headroom_target);
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

double ResidencyGovernor::prediction_confidence(const ResidencyObject& object, std::uint64_t epoch) const noexcept {
    if (object.use_count < config_.minimum_prediction_samples || !(object.cost.reuse_interval > 0.0) ||
        !std::isfinite(object.cost.reuse_interval) || !std::isfinite(object.cost.reuse_deviation)) {
        return 0.0;
    }

    const auto mean = (std::max)(1.0, object.cost.reuse_interval);
    if (epoch > object.last_use_epoch) {
        const auto age = static_cast<double>(epoch - object.last_use_epoch);
        const auto stale_after = mean * static_cast<double>(config_.prediction_stale_intervals);
        if (age > stale_after) {
            return 0.0;
        }
    }

    const auto normalized_deviation = (std::min)(1.0, object.cost.reuse_deviation / mean);
    const auto stability = 1.0 - normalized_deviation;
    const auto sample_span = static_cast<double>((std::max<std::uint32_t>)(config_.minimum_prediction_samples, 6U));
    const auto sample_factor = (std::min)(1.0, static_cast<double>(object.use_count - 1) / sample_span);
    return (std::max)(0.0, (std::min)(1.0, stability * sample_factor));
}

std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(
    const ResidencyObject& object,
    std::uint64_t epoch) const {
    if (object.use_count < 2 || !(object.cost.reuse_interval > 0.0) || !std::isfinite(object.cost.reuse_interval)) {
        return std::nullopt;
    }

    const auto rounded = (std::max)(1.0, std::round(object.cost.reuse_interval));
    const auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
    const auto interval = rounded >= static_cast<double>(max_u64)
        ? max_u64
        : static_cast<std::uint64_t>(rounded);
    if (!interval || object.last_use_epoch > max_u64 - interval) {
        return max_u64;
    }

    auto next = object.last_use_epoch + interval;
    if (next <= epoch && interval != max_u64) {
        const auto elapsed = epoch - object.last_use_epoch;
        const auto cycles = elapsed / interval + 1;
        if (cycles > (max_u64 - object.last_use_epoch) / interval) {
            return max_u64;
        }
        next = object.last_use_epoch + cycles * interval;
    }
    return next;
}

std::optional<std::uint64_t> ResidencyGovernor::predicted_next_use(ResidencyId id, std::uint64_t epoch) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? std::nullopt : predicted_next_use(it->second, epoch);
}

std::optional<ResidencyPrediction> ResidencyGovernor::prediction(ResidencyId id, std::uint64_t epoch) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return std::nullopt;
    }
    const auto predicted = predicted_next_use(it->second, epoch);
    if (!predicted) {
        return std::nullopt;
    }
    return ResidencyPrediction{*predicted, prediction_confidence(it->second, epoch)};
}

std::vector<ResidencyAction> ResidencyGovernor::plan_evictions(std::uint64_t epoch) const {
    const auto required = bytes_to_free();
    if (!required) {
        return {};
    }

    std::vector<ResidencyAction> candidates;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Resident ||
            !eviction_fence_safe(object)) {
            continue;
        }

        const auto age = epoch >= object.last_use_epoch ? epoch - object.last_use_epoch : 0;
        if (age < config_.minimum_residency_age_epochs) {
            continue;
        }
        if (object.last_miss_epoch && epoch >= object.last_miss_epoch &&
            epoch - object.last_miss_epoch < config_.post_miss_grace_epochs) {
            continue;
        }

        const auto predicted = predicted_next_use(object, epoch);
        const auto confidence = prediction_confidence(object, epoch);
        if (predicted && confidence >= config_.minimum_prefetch_confidence &&
            *predicted <= epoch + config_.prefetch_horizon_epochs) {
            continue;
        }

        const auto age_factor = object.use_count && object.cost.reuse_interval > 0.0
            ? static_cast<double>(age) / object.cost.reuse_interval
            : 4.0;
        const auto kib = static_cast<double>((std::max<std::uint64_t>)(1, object.cost.bytes / 1024));
        const auto uncertainty_factor = 0.45 + 0.55 * confidence;
        const auto miss_penalty = 1.0 + static_cast<double>(object.demand_miss_count) * 0.75;
        const auto score = age_factor * std::sqrt(kib) * uncertainty_factor /
            ((1.0 + (std::max)(0.0, object.cost.reload_ms)) * miss_penalty);

        candidates.push_back({
            ResidencyAction::Type::Evict,
            id,
            object.cost.bytes,
            predicted.value_or(0),
            score,
            object.last_use_queue,
            object.last_use_fence});
    }

    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.bytes != right.bytes) {
            return left.bytes > right.bytes;
        }
        return left.object < right.object;
    });

    std::vector<ResidencyAction> result;
    std::uint64_t planned{};
    for (const auto& action : candidates) {
        result.push_back(action);
        planned = action.bytes > (std::numeric_limits<std::uint64_t>::max)() - planned
            ? (std::numeric_limits<std::uint64_t>::max)()
            : planned + action.bytes;
        if (planned >= required) {
            break;
        }
    }
    return result;
}

std::vector<ResidencyAction> ResidencyGovernor::plan_promotions(std::uint64_t epoch) const {
    if (!speculative_promotions_allowed() || !budget_) {
        return {};
    }

    std::vector<ResidencyAction> candidates;
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) {
            continue;
        }

        const auto predicted = predicted_next_use(object, epoch);
        const auto confidence = prediction_confidence(object, epoch);
        if (!predicted || confidence < config_.minimum_prefetch_confidence ||
            *predicted > epoch + config_.prefetch_horizon_epochs) {
            continue;
        }

        const auto distance = *predicted > epoch ? *predicted - epoch : 0;
        const auto score = confidence * (1.0 + (std::max)(0.0, object.cost.reload_ms)) /
            (1.0 + static_cast<double>(distance));
        candidates.push_back({ResidencyAction::Type::MakeResident, id, object.cost.bytes, *predicted, score});
    }

    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.predicted_use_epoch != right.predicted_use_epoch) {
            return left.predicted_use_epoch < right.predicted_use_epoch;
        }
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.object < right.object;
    });

    const auto ceiling = ratio_bytes(budget_, config_.promotion_ceiling);
    const auto headroom_ceiling = config_.minimum_headroom_bytes && budget_ > config_.minimum_headroom_bytes
        ? budget_ - config_.minimum_headroom_bytes
        : ceiling;
    const auto target = (std::min)(ceiling, headroom_ceiling);

    std::uint64_t projected = usage_;
    std::vector<ResidencyAction> result;
    for (const auto& action : candidates) {
        if (action.bytes > target || projected > target - action.bytes) {
            continue;
        }
        result.push_back(action);
        projected += action.bytes;
    }
    return result;
}

std::optional<ResidencyAction> ResidencyGovernor::require_resident(ResidencyId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return std::nullopt;
    }
    const auto& object = it->second;
    if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) {
        return std::nullopt;
    }
    return ResidencyAction{ResidencyAction::Type::MakeResident, id, object.cost.bytes, 0, 1.0};
}

std::vector<ResidencyAction> ResidencyGovernor::plan(std::uint64_t epoch) const {
    return pressure_ == PressureState::Normal ? plan_promotions(epoch) : plan_evictions(epoch);
}

ResidencyPlanSummary ResidencyGovernor::plan_summary(std::uint64_t epoch) const {
    ResidencyPlanSummary summary{
        .pressure = pressure_,
        .budget = budget_,
        .usage = usage_,
        .target_usage = target_usage_bytes(),
        .bytes_to_free = bytes_to_free()};
    for (const auto& action : plan_evictions(epoch)) {
        summary.bytes_planned_to_free += action.bytes;
    }
    for (const auto& action : plan_promotions(epoch)) {
        summary.bytes_planned_to_promote += action.bytes;
    }
    summary.shortfall = summary.bytes_planned_to_free < summary.bytes_to_free;
    return summary;
}

std::optional<ResidencyObject> ResidencyGovernor::find(ResidencyId id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? std::nullopt : std::optional(it->second);
}

void ResidencyGovernor::record_eviction(ResidencyId id, bool later_reloaded, std::uint64_t epoch) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return;
    }
    auto& object = it->second;
    if (epoch) {
        object.last_evicted_epoch = epoch;
    }
    metrics_.bytes_evicted += object.cost.bytes;
    if (later_reloaded) {
        ++metrics_.false_evictions;
    } else {
        ++metrics_.useful_evictions;
    }
}

void ResidencyGovernor::record_resident(ResidencyId id, bool late, std::uint64_t epoch) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return;
    }

    auto& object = it->second;
    if (epoch) {
        object.last_resident_epoch = epoch;
    }
    ++metrics_.reloads;
    metrics_.bytes_made_resident += object.cost.bytes;

    if (!late) {
        return;
    }

    ++metrics_.late_residency;
    ++object.demand_miss_count;
    object.last_miss_epoch = epoch;
    if (prediction_confidence(object, epoch) >= config_.minimum_prefetch_confidence) {
        ++metrics_.predictable_misses;
    } else {
        ++metrics_.compulsory_misses;
    }
}

}  // namespace arc
