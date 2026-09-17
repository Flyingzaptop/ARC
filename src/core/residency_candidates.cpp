#include "arc/residency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace arc {

std::vector<ResidencyAction> ResidencyGovernor::eviction_candidates(std::uint64_t epoch) const {
    std::vector<ResidencyAction> candidates;
    candidates.reserve(objects_.size());

    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe ||
            object.state != ResidencyState::Resident ||
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
        const auto prediction_guard = pressure_ == PressureState::Emergency
            ? config_.prefetch_horizon_epochs
            : (std::max)(config_.prefetch_horizon_epochs, config_.eviction_prediction_guard_epochs);
        if (predicted && confidence >= config_.minimum_prefetch_confidence &&
            *predicted <= epoch + prediction_guard) {
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
        if (left.score != right.score) return left.score > right.score;
        if (left.bytes != right.bytes) return left.bytes > right.bytes;
        return left.object < right.object;
    });
    return candidates;
}

std::vector<ResidencyAction> ResidencyGovernor::promotion_candidates(std::uint64_t epoch) const {
    if (!speculative_promotions_allowed() || !budget_) {
        return {};
    }

    std::vector<ResidencyAction> candidates;
    candidates.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Evicted) {
            continue;
        }

        const auto predicted = predicted_next_use(object, epoch);
        const auto confidence = prediction_confidence(object, epoch);
        const bool actionable_prediction = predicted &&
            confidence >= config_.minimum_prefetch_confidence &&
            *predicted <= epoch + config_.prefetch_horizon_epochs;

        if (actionable_prediction) {
            const auto distance = *predicted > epoch ? *predicted - epoch : 0;
            const auto score = confidence * (1.0 + (std::max)(0.0, object.cost.reload_ms)) /
                (1.0 + static_cast<double>(distance));
            candidates.push_back({ResidencyAction::Type::MakeResident, id, object.cost.bytes, *predicted, score});
            continue;
        }

        // A resource with no current actionable reuse forecast must not remain
        // evicted forever once memory pressure has fully recovered. Admit it as
        // a deliberately low-priority background restore candidate. Known
        // imminent reuse always sorts ahead of this path, and the global
        // headroom planner still enforces promotion_ceiling/reserve limits.
        // Waiting at least one epoch after eviction prevents same-tick churn.
        if (object.last_evicted_epoch && epoch <= object.last_evicted_epoch) {
            continue;
        }
        const double reload = 1.0 + (std::max)(0.0, object.cost.reload_ms);
        const double age = object.last_evicted_epoch && epoch > object.last_evicted_epoch
            ? static_cast<double>(epoch - object.last_evicted_epoch)
            : 1.0;
        const double background_score = (0.01 + (std::min)(0.04, age * 0.0005)) * reload;
        candidates.push_back({
            ResidencyAction::Type::MakeResident,
            id,
            object.cost.bytes,
            (std::numeric_limits<std::uint64_t>::max)(),
            background_score});
    }

    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.predicted_use_epoch != right.predicted_use_epoch) {
            return left.predicted_use_epoch < right.predicted_use_epoch;
        }
        if (left.score != right.score) return left.score > right.score;
        return left.object < right.object;
    });
    return candidates;
}

}  // namespace arc
