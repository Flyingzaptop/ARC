#include "arc/global_action_arbiter.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

double clamp01(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return std::clamp(v, 0.0, 1.0);
}

double pressure_of(const FrameBudgetSample& frame) noexcept {
    if (frame.local_budget_bytes == 0) return 0.0;
    return std::clamp(
        static_cast<double>(frame.local_usage_bytes) /
            static_cast<double>(frame.local_budget_bytes),
        0.0,
        4.0);
}

} // namespace

GlobalActionArbiter::GlobalActionArbiter(GlobalActionArbiterConfig config)
    : config_(config) {
    config_.memory_pressure_enter = std::clamp(config_.memory_pressure_enter, 0.0, 1.0);
    config_.memory_pressure_emergency = std::clamp(
        std::max(config_.memory_pressure_enter, config_.memory_pressure_emergency), 0.0, 1.0);
    config_.visual_cost_floor = std::max(1e-6, config_.visual_cost_floor);
}

bool GlobalActionArbiter::memory_has_work(const LiveRuntimePlan& plan) const noexcept {
    if (plan.pressure != PressureState::Normal) {
        return !plan.pressure_relief.arbitration.actions.empty();
    }
    return !plan.transition_prefetch.empty() || !plan.restore.arbitration.actions.empty();
}

double GlobalActionArbiter::memory_score(const LiveRuntimePlan& plan, double pressure) const noexcept {
    if (!memory_has_work(plan)) return 0.0;
    if (plan.pressure == PressureState::Normal) {
        // Normal-state restore/prefetch is useful but intentionally subordinate
        // to an active frame deficit.
        return 0.25;
    }

    const auto requested = static_cast<double>(std::max<std::uint64_t>(1, plan.pressure_relief.arbitration.requested_bytes));
    const auto planned = static_cast<double>(plan.pressure_relief.arbitration.planned_bytes);
    const double relief_fraction = std::clamp(planned / requested, 0.0, 1.5);
    const double urgency = pressure >= config_.memory_pressure_emergency
        ? 2.0
        : 0.75 + 1.25 * clamp01((pressure - config_.memory_pressure_enter) /
              std::max(1e-6, config_.memory_pressure_emergency - config_.memory_pressure_enter));
    const double penalty = std::max(0.0, plan.pressure_relief.arbitration.total_penalty);
    return urgency * relief_fraction / (1.0 + penalty);
}

double GlobalActionArbiter::quality_score(
    const QualityDecision& quality,
    const FrameBudgetSample& frame) const noexcept {
    if (quality.kind == QualityDecisionKind::None || quality.plan.actions.empty()) return 0.0;
    const double visual = std::max(config_.visual_cost_floor, quality.plan.estimated_visual_cost);

    if (quality.kind == QualityDecisionKind::Restore) {
        const double headroom = std::max(0.0, frame.target_frame_ms - frame.frame_ms);
        return headroom * visual;
    }

    const double deficit = std::max(0.01, frame.frame_ms - frame.target_frame_ms);
    const double gain_fraction = std::clamp(quality.plan.planned_gain_ms / deficit, 0.0, 2.0);
    return gain_fraction / visual;
}

GlobalArbitrationDecision GlobalActionArbiter::decide(
    const LiveRuntimePlan& memory,
    const QualityDecision& quality,
    const FrameBudgetSample& frame) const noexcept {
    GlobalArbitrationDecision out{};
    const double pressure = pressure_of(frame);
    const bool frame_over = frame.target_frame_ms > 0.0 && frame.frame_ms > frame.target_frame_ms;
    out.memory_has_work = memory_has_work(memory);
    out.quality_has_work = quality.kind != QualityDecisionKind::None && !quality.plan.actions.empty();
    out.memory_emergency = pressure >= config_.memory_pressure_emergency || memory.pressure == PressureState::Emergency;
    out.memory_score = memory_score(memory, pressure);
    out.quality_score = quality_score(quality, frame);

    if (!out.memory_has_work && !out.quality_has_work) return out;

    if (out.memory_emergency) {
        out.execute_memory = out.memory_has_work;
        // Capacity emergency gets one physical relief action first. Generic
        // quality may join only when the specialized memory plan cannot fully
        // meet its target and the quality action itself frees physical bytes.
        const bool memory_shortfall = memory.pressure_relief.arbitration.shortfall;
        const bool quality_relieves_memory = out.quality_has_work &&
            quality.kind == QualityDecisionKind::Degrade &&
            quality.plan.planned_memory_freed_bytes != 0;
        out.execute_quality = quality_relieves_memory &&
            (!out.execute_memory || memory_shortfall) && config_.allow_combined;
        out.choice = out.execute_quality
            ? (out.execute_memory ? GlobalArbitrationChoice::Combined : GlobalArbitrationChoice::Quality)
            : (out.execute_memory ? GlobalArbitrationChoice::Memory : GlobalArbitrationChoice::None);
        return out;
    }

    if (frame_over) {
        // Never spend frame headroom on normal residency restoration while the
        // renderer is already missing its target.
        const bool memory_is_relief = memory.pressure != PressureState::Normal && out.memory_has_work;
        if (!out.quality_has_work) {
            out.execute_memory = memory_is_relief;
            out.choice = out.execute_memory ? GlobalArbitrationChoice::Memory : GlobalArbitrationChoice::None;
            return out;
        }

        if (!memory_is_relief) {
            out.execute_quality = true;
            out.choice = GlobalArbitrationChoice::Quality;
            return out;
        }

        // Moderate simultaneous frame+memory pressure: allow both when both
        // policies carry material utility. Otherwise choose the stronger one.
        if (config_.allow_combined && out.memory_score > 0.05 && out.quality_score > 0.05) {
            out.execute_memory = true;
            out.execute_quality = true;
            out.choice = GlobalArbitrationChoice::Combined;
        } else if (out.quality_score >= out.memory_score) {
            out.execute_quality = true;
            out.choice = GlobalArbitrationChoice::Quality;
        } else {
            out.execute_memory = true;
            out.choice = GlobalArbitrationChoice::Memory;
        }
        return out;
    }

    // Headroom path: restore quality first only when memory is not pressured;
    // normal residency restoration/prefetch may coexist. Quality controller's
    // own VRAM guard has already filtered unsafe restores.
    if (quality.kind == QualityDecisionKind::Restore && out.quality_has_work) {
        out.execute_quality = true;
        out.execute_memory = out.memory_has_work && memory.pressure == PressureState::Normal;
        out.choice = GlobalArbitrationChoice::Restore;
        return out;
    }

    if (out.memory_has_work) {
        out.execute_memory = true;
        out.choice = GlobalArbitrationChoice::Memory;
    }
    return out;
}

} // namespace arc
