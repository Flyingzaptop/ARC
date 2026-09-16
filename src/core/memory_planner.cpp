#include "arc/memory_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace arc {
namespace {

bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
        ? (std::numeric_limits<std::uint64_t>::max)()
        : left + right;
}

}  // namespace

GlobalMemoryPlanner::GlobalMemoryPlanner(GlobalMemoryPlannerConfig config)
    : config_(config), arbiter_(config.arbiter) {
    if (!finite_unit(config_.unknown_prediction_confidence)) config_.unknown_prediction_confidence = 0.50;
    if (!std::isfinite(config_.minimum_reload_risk_factor) || config_.minimum_reload_risk_factor < 0.0) {
        config_.minimum_reload_risk_factor = 0.10;
    }
}

GlobalMemoryPlan GlobalMemoryPlanner::plan_pressure_relief(
    const ResidencyGovernor& residency,
    const TextureQualityGovernor& textures,
    std::uint64_t epoch) const {
    GlobalMemoryPlan result{};
    result.requested_bytes = residency.bytes_to_free();
    if (!result.requested_bytes) {
        return result;
    }

    std::vector<MemoryActionCandidate> candidates;

    const auto residency_actions = residency.plan_evictions(epoch);
    candidates.reserve(residency_actions.size() + textures.config().max_demotions_per_plan);
    for (const auto& action : residency_actions) {
        const auto object = residency.find(action.object);
        if (!object || object->resource == 0) continue;
        const auto prediction = residency.prediction(action.object, epoch);
        const auto prediction_confidence = prediction ? prediction->confidence : config_.unknown_prediction_confidence;
        double urgency = config_.minimum_reload_risk_factor;
        if (prediction && prediction->epoch > epoch) {
            const auto distance = static_cast<double>(prediction->epoch - epoch);
            urgency = (std::max)(urgency, prediction->confidence / (1.0 + distance));
        }
        const auto latency_risk = (std::max)(0.0, object->cost.reload_ms) * urgency;
        candidates.push_back(MemoryActionCandidate{
            .kind = MemoryActionKind::EvictResource,
            .resource = object->resource,
            .subject = action.object,
            .sequence = 0,
            .bytes_freed = action.bytes,
            .quality_loss = 0.0,
            .latency_risk_ms = latency_risk,
            .confidence = prediction_confidence,
            .eligible = true,
        });
        result.residency_candidate_bytes = saturating_add(result.residency_candidate_bytes, action.bytes);
    }

    const auto texture_actions = textures.plan_demotions(result.requested_bytes, epoch);
    for (const auto& action : texture_actions) {
        const auto loss = action.quality_delta < 0.0 ? -action.quality_delta : 0.0;
        candidates.push_back(MemoryActionCandidate{
            .kind = MemoryActionKind::DemoteTexture,
            .resource = action.resource,
            .subject = action.texture,
            .sequence = action.from_level,
            .bytes_freed = action.bytes_delta,
            .quality_loss = loss,
            .latency_risk_ms = 0.0,
            .confidence = 1.0,
            .eligible = true,
        });
        result.texture_candidate_bytes = saturating_add(result.texture_candidate_bytes, action.bytes_delta);
    }

    result.arbitration = arbiter_.plan(result.requested_bytes, candidates);
    return result;
}

GlobalMemoryRestorePlan GlobalMemoryPlanner::plan_headroom_restore(
    const ResidencyGovernor& residency,
    const TextureQualityGovernor& textures,
    std::uint64_t epoch,
    std::uint64_t headroom_bytes) const {
    GlobalMemoryRestorePlan result{};
    result.headroom_bytes = headroom_bytes;
    if (!headroom_bytes) {
        return result;
    }

    std::vector<MemoryRestoreCandidate> candidates;

    const auto residency_actions = residency.plan_promotions(epoch);
    candidates.reserve(residency_actions.size() + textures.config().max_promotions_per_plan);
    for (const auto& action : residency_actions) {
        const auto object = residency.find(action.object);
        if (!object || object->resource == 0) continue;
        const auto prediction = residency.prediction(action.object, epoch);
        const auto confidence = prediction ? prediction->confidence : config_.unknown_prediction_confidence;
        double latency_benefit = (std::max)(0.0, object->cost.reload_ms) * confidence;
        if (prediction && prediction->epoch > epoch) {
            const auto distance = static_cast<double>(prediction->epoch - epoch);
            latency_benefit /= (1.0 + distance);
        }
        candidates.push_back(MemoryRestoreCandidate{
            .kind = MemoryRestoreKind::MakeResident,
            .resource = object->resource,
            .subject = action.object,
            .sequence = 0,
            .bytes_cost = action.bytes,
            .quality_gain = 0.0,
            .latency_benefit_ms = latency_benefit,
            .confidence = confidence,
            .eligible = true,
        });
        result.residency_candidate_bytes = saturating_add(result.residency_candidate_bytes, action.bytes);
    }

    const auto texture_actions = textures.plan_promotions(headroom_bytes, epoch);
    std::unordered_map<ResourceId, std::uint32_t> resource_sequences;
    for (const auto& action : texture_actions) {
        const auto sequence = resource_sequences[action.resource]++;
        candidates.push_back(MemoryRestoreCandidate{
            .kind = MemoryRestoreKind::PromoteTexture,
            .resource = action.resource,
            .subject = action.texture,
            .sequence = sequence,
            .bytes_cost = action.bytes_delta,
            .quality_gain = action.quality_delta > 0.0 ? action.quality_delta : 0.0,
            .latency_benefit_ms = 0.0,
            .confidence = 1.0,
            .eligible = true,
        });
        result.texture_candidate_bytes = saturating_add(result.texture_candidate_bytes, action.bytes_delta);
    }

    result.arbitration = arbiter_.plan_restore(headroom_bytes, candidates);
    return result;
}

}  // namespace arc
