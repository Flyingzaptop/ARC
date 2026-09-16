#include "arc/memory_arbiter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace arc {
namespace {

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

struct ResourcePlanState {
    bool whole_resource_selected{};
    bool any_demotion_selected{};
    std::uint32_t next_demotion_sequence{};
};

}  // namespace

MemoryArbiter::MemoryArbiter(MemoryArbiterConfig config) : config_(config) {
    if (!finite_nonnegative(config_.quality_weight)) config_.quality_weight = 1.0;
    if (!finite_nonnegative(config_.latency_weight)) config_.latency_weight = 1.0;
    if (!finite_nonnegative(config_.uncertainty_weight)) config_.uncertainty_weight = 0.10;
    if (config_.max_actions == 0) config_.max_actions = 1;
}

double MemoryArbiter::penalty(const MemoryActionCandidate& candidate) const noexcept {
    if (candidate.bytes_freed == 0 || !candidate.eligible) {
        return std::numeric_limits<double>::infinity();
    }
    const auto quality = finite_nonnegative(candidate.quality_loss) ? candidate.quality_loss : std::numeric_limits<double>::infinity();
    const auto latency = finite_nonnegative(candidate.latency_risk_ms) ? candidate.latency_risk_ms : std::numeric_limits<double>::infinity();
    const auto confidence = std::isfinite(candidate.confidence)
        ? (std::max)(0.0, (std::min)(1.0, candidate.confidence))
        : 0.0;
    return config_.quality_weight * quality +
        config_.latency_weight * latency +
        config_.uncertainty_weight * (1.0 - confidence);
}

MemoryArbiterPlan MemoryArbiter::plan(
    std::uint64_t bytes_to_free,
    const std::vector<MemoryActionCandidate>& candidates) const {
    MemoryArbiterPlan result{};
    result.requested_bytes = bytes_to_free;
    if (bytes_to_free == 0) {
        return result;
    }

    std::unordered_map<ResourceId, ResourcePlanState> state;
    std::unordered_set<std::size_t> selected;

    while (result.planned_bytes < bytes_to_free && result.actions.size() < config_.max_actions) {
        std::optional<std::size_t> best_index;
        double best_per_byte = std::numeric_limits<double>::infinity();
        double best_penalty = std::numeric_limits<double>::infinity();

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (selected.contains(index)) continue;
            const auto& candidate = candidates[index];
            if (!candidate.eligible || candidate.resource == 0 || candidate.subject == 0 || candidate.bytes_freed == 0) continue;

            const auto plan_state = state.find(candidate.resource);
            const ResourcePlanState empty{};
            const auto& resource_state = plan_state == state.end() ? empty : plan_state->second;
            if (candidate.kind == MemoryActionKind::EvictResource) {
                if (resource_state.whole_resource_selected || resource_state.any_demotion_selected) continue;
            } else {
                if (resource_state.whole_resource_selected || candidate.sequence != resource_state.next_demotion_sequence) continue;
            }

            const auto total = penalty(candidate);
            if (!std::isfinite(total)) continue;
            const auto per_byte = total / static_cast<double>(candidate.bytes_freed);
            const bool better = !best_index || per_byte < best_per_byte ||
                (per_byte == best_per_byte && candidate.bytes_freed > candidates[*best_index].bytes_freed) ||
                (per_byte == best_per_byte && candidate.bytes_freed == candidates[*best_index].bytes_freed && candidate.resource < candidates[*best_index].resource);
            if (better) {
                best_index = index;
                best_per_byte = per_byte;
                best_penalty = total;
            }
        }

        if (!best_index) break;
        const auto& candidate = candidates[*best_index];
        selected.insert(*best_index);
        auto& resource_state = state[candidate.resource];
        if (candidate.kind == MemoryActionKind::EvictResource) {
            resource_state.whole_resource_selected = true;
        } else {
            resource_state.any_demotion_selected = true;
            ++resource_state.next_demotion_sequence;
        }

        result.actions.push_back(MemoryArbiterAction{candidate, best_penalty, best_per_byte});
        if (candidate.bytes_freed > (std::numeric_limits<std::uint64_t>::max)() - result.planned_bytes) {
            result.planned_bytes = (std::numeric_limits<std::uint64_t>::max)();
        } else {
            result.planned_bytes += candidate.bytes_freed;
        }
        result.total_penalty += best_penalty;
    }

    result.shortfall = result.planned_bytes < result.requested_bytes;
    return result;
}

}  // namespace arc
