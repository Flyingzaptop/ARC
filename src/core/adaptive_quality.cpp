#include "arc/adaptive_quality.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace arc {
namespace {
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

double clamp01(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

double pressure(const FrameBudgetSample& sample) noexcept {
    if (sample.local_budget_bytes == 0) return 0.0;
    return std::clamp(
        static_cast<double>(sample.local_usage_bytes) /
            static_cast<double>(sample.local_budget_bytes),
        0.0,
        4.0);
}

std::uint64_t required_memory_relief(
    const FrameBudgetSample& sample,
    double target_fraction) noexcept {
    if (sample.local_budget_bytes == 0) return 0;
    const long double target =
        static_cast<long double>(sample.local_budget_bytes) *
        static_cast<long double>(std::clamp(target_fraction, 0.0, 1.0));
    if (static_cast<long double>(sample.local_usage_bytes) <= target) return 0;
    const long double relief = static_cast<long double>(sample.local_usage_bytes) - target;
    return static_cast<std::uint64_t>(std::ceil(relief));
}
} // namespace

double ResourceImportanceEstimator::score(const ResourceImportanceSample& sample) noexcept {
    const double screen = clamp01(sample.screen_coverage);
    const double visibility = clamp01(sample.visibility);
    const double motion = clamp01(sample.motion_salience);
    const double semantic = clamp01(sample.semantic_importance);
    const double near_factor = 1.0 - clamp01(sample.normalized_distance);

    const double weighted =
        0.30 * screen +
        0.24 * visibility +
        0.12 * motion +
        0.24 * semantic +
        0.10 * near_factor;
    return std::clamp(weighted, 0.0, 1.0);
}

BottleneckClass FrameBottleneckAnalyzer::classify(const FrameBudgetSample& sample) noexcept {
    const double memory_pressure = pressure(sample);
    if (memory_pressure >= 0.97) return BottleneckClass::MemoryCapacity;

    struct Candidate { BottleneckClass kind; double score; };
    const Candidate candidates[] = {
        {BottleneckClass::MemoryBandwidth, clamp01(sample.memory_bandwidth_fraction)},
        {BottleneckClass::Lighting, clamp01(sample.lighting_pressure)},
        {BottleneckClass::Shadow, clamp01(sample.shadow_pressure)},
        {BottleneckClass::Raster, clamp01(sample.raster_pressure)},
        {BottleneckClass::Geometry, clamp01(sample.geometry_pressure)},
    };

    Candidate best{BottleneckClass::UnknownGpu, clamp01(sample.gpu_busy_fraction) * 0.75};
    for (const auto& candidate : candidates) {
        if (candidate.score > best.score) best = candidate;
    }

    const bool over_budget = sample.frame_ms > sample.target_frame_ms;
    if (!over_budget && memory_pressure < 0.90) return BottleneckClass::Balanced;
    if (memory_pressure >= 0.90 && best.score < 0.92) return BottleneckClass::MemoryCapacity;
    if (best.score < 0.55) return BottleneckClass::UnknownGpu;
    return best.kind;
}

AdaptiveQualityOptimizer::AdaptiveQualityOptimizer(AdaptiveQualityConfig config)
    : config_(config) {
    config_.minimum_confidence = std::clamp(config_.minimum_confidence, 0.0, 1.0);
    config_.minimum_gain_ms = std::max(0.0, config_.minimum_gain_ms);
    config_.visual_cost_floor = std::max(1e-6, config_.visual_cost_floor);
    config_.memory_pressure_enter = std::clamp(config_.memory_pressure_enter, 0.0, 1.0);
    config_.memory_pressure_emergency = std::clamp(
        std::max(config_.memory_pressure_enter, config_.memory_pressure_emergency), 0.0, 1.0);
    config_.restoration_headroom_ms = std::max(0.0, config_.restoration_headroom_ms);
    config_.memory_value_ms_per_gib = std::max(0.0, config_.memory_value_ms_per_gib);
    config_.minimum_domain_affinity = std::clamp(config_.minimum_domain_affinity, 0.0, 1.0);
    config_.unknown_gpu_domain_affinity = std::clamp(config_.unknown_gpu_domain_affinity, 0.0, 1.0);
    config_.max_actions_per_plan = std::max<std::uint32_t>(1, config_.max_actions_per_plan);
}

double AdaptiveQualityOptimizer::domain_affinity(
    BottleneckClass bottleneck,
    QualityDomain domain) const noexcept {
    if (domain == QualityDomain::Temporal) return 1.0;

    switch (bottleneck) {
    case BottleneckClass::Balanced:
        return 0.0;
    case BottleneckClass::MemoryCapacity:
        switch (domain) {
        case QualityDomain::Texture: return 1.00;
        case QualityDomain::Bandwidth: return 0.95;
        case QualityDomain::Shadow: return 0.65;
        case QualityDomain::Lighting: return 0.16;
        case QualityDomain::Raster: return 0.12;
        case QualityDomain::Geometry: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::MemoryBandwidth:
        switch (domain) {
        case QualityDomain::Bandwidth: return 1.00;
        case QualityDomain::Texture: return 0.95;
        case QualityDomain::Shadow: return 0.35;
        case QualityDomain::Lighting: return 0.16;
        case QualityDomain::Raster: return 0.12;
        case QualityDomain::Geometry: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::Raster:
        switch (domain) {
        case QualityDomain::Raster: return 1.00;
        case QualityDomain::Geometry: return 0.35;
        case QualityDomain::Shadow: return 0.25;
        case QualityDomain::Lighting: return 0.14;
        case QualityDomain::Bandwidth: return 0.12;
        case QualityDomain::Texture: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::Geometry:
        switch (domain) {
        case QualityDomain::Geometry: return 1.00;
        case QualityDomain::Raster: return 0.30;
        case QualityDomain::Shadow: return 0.14;
        case QualityDomain::Lighting: return 0.12;
        case QualityDomain::Bandwidth: return 0.12;
        case QualityDomain::Texture: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::Lighting:
        switch (domain) {
        case QualityDomain::Lighting: return 1.00;
        case QualityDomain::Shadow: return 0.45;
        case QualityDomain::Raster: return 0.14;
        case QualityDomain::Geometry: return 0.12;
        case QualityDomain::Bandwidth: return 0.12;
        case QualityDomain::Texture: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::Shadow:
        switch (domain) {
        case QualityDomain::Shadow: return 1.00;
        case QualityDomain::Lighting: return 0.25;
        case QualityDomain::Raster: return 0.14;
        case QualityDomain::Geometry: return 0.12;
        case QualityDomain::Bandwidth: return 0.12;
        case QualityDomain::Texture: return 0.12;
        case QualityDomain::Temporal: return 1.0;
        }
        break;
    case BottleneckClass::UnknownGpu:
        return config_.unknown_gpu_domain_affinity;
    }
    return 0.0;
}

double AdaptiveQualityOptimizer::utility(
    const QualityActionCandidate& candidate,
    double memory_pressure,
    BottleneckClass bottleneck) const noexcept {
    const double visual = std::max(config_.visual_cost_floor, candidate.visual_cost);
    const double memory_gib = static_cast<double>(candidate.memory_freed_bytes) / kGiB;
    const double memory_weight = memory_pressure >= config_.memory_pressure_enter
        ? config_.memory_value_ms_per_gib * (1.0 + 3.0 * (memory_pressure - config_.memory_pressure_enter))
        : 0.0;
    const double benefit = candidate.expected_ms_gain + memory_gib * memory_weight;
    const double affinity = domain_affinity(bottleneck, candidate.domain);
    return candidate.confidence * benefit * affinity / visual;
}

AdaptiveQualityPlan AdaptiveQualityOptimizer::plan_degrade(
    const FrameBudgetSample& sample,
    const std::vector<QualityActionCandidate>& candidates) const {
    AdaptiveQualityPlan plan{};
    plan.bottleneck = FrameBottleneckAnalyzer::classify(sample);
    plan.frame_deficit_ms = std::max(0.0, sample.frame_ms - sample.target_frame_ms);

    const double memory_pressure = pressure(sample);
    const bool memory_emergency = memory_pressure >= config_.memory_pressure_emergency;
    const std::uint64_t memory_relief_target = memory_emergency
        ? required_memory_relief(sample, config_.memory_pressure_enter)
        : 0;
    if (plan.bottleneck == BottleneckClass::Balanced && !memory_emergency) return plan;

    struct Ranked {
        QualityActionCandidate candidate;
        double utility{};
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        if (!candidate.reversible) continue;
        if (candidate.confidence < config_.minimum_confidence) continue;
        if (candidate.expected_ms_gain < config_.minimum_gain_ms && candidate.memory_freed_bytes == 0) continue;
        const bool is_temporal = candidate.temporal_assist || candidate.domain == QualityDomain::Temporal;
        if (is_temporal && !config_.allow_temporal_assist) continue;

        const double affinity = is_temporal ? 1.0 : domain_affinity(plan.bottleneck, candidate.domain);
        if (!memory_emergency && !is_temporal && affinity + 1e-12 < config_.minimum_domain_affinity) continue;
        ranked.push_back({candidate, utility(candidate, memory_pressure, plan.bottleneck)});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.utility != b.utility) return a.utility > b.utility;
        if (a.candidate.sequence != b.candidate.sequence) return a.candidate.sequence < b.candidate.sequence;
        return a.candidate.id < b.candidate.id;
    });

    std::unordered_map<std::uint64_t, std::uint32_t> next_sequence;
    for (const auto& item : ranked) {
        const auto it = next_sequence.find(item.candidate.id);
        if (it == next_sequence.end()) next_sequence.emplace(item.candidate.id, item.candidate.sequence);
        else it->second = std::min(it->second, item.candidate.sequence);
    }

    const double target_gain = plan.frame_deficit_ms;
    for (const auto& item : ranked) {
        if (plan.actions.size() >= config_.max_actions_per_plan) break;

        auto expected = next_sequence.find(item.candidate.id);
        if (expected == next_sequence.end() || item.candidate.sequence != expected->second) continue;

        plan.actions.push_back(item.candidate);
        ++expected->second;
        plan.planned_gain_ms += item.candidate.expected_ms_gain;
        plan.estimated_visual_cost += item.candidate.visual_cost;
        plan.planned_memory_freed_bytes += item.candidate.memory_freed_bytes;
        plan.temporal_used = plan.temporal_used || item.candidate.temporal_assist || item.candidate.domain == QualityDomain::Temporal;

        const bool frame_satisfied = target_gain <= 0.0 || plan.planned_gain_ms >= target_gain;
        const bool memory_satisfied = !memory_emergency || plan.planned_memory_freed_bytes >= memory_relief_target;
        if (frame_satisfied && memory_satisfied) break;
    }

    const bool frame_shortfall = target_gain > 0.0 && plan.planned_gain_ms + 1e-9 < target_gain;
    const bool memory_shortfall = memory_emergency && plan.planned_memory_freed_bytes < memory_relief_target;
    plan.shortfall = frame_shortfall || memory_shortfall;
    return plan;
}

AdaptiveQualityPlan AdaptiveQualityOptimizer::plan_restore(
    const FrameBudgetSample& sample,
    const std::vector<QualityActionCandidate>& active_actions) const {
    AdaptiveQualityPlan plan{};
    plan.bottleneck = FrameBottleneckAnalyzer::classify(sample);

    // GPU headroom is not permission to increase quality while local VRAM is
    // still under pressure.  Memory safety wins over visual restoration.
    if (pressure(sample) >= config_.memory_pressure_enter) return plan;

    const double headroom = sample.target_frame_ms - sample.frame_ms;
    if (headroom < config_.restoration_headroom_ms) return plan;

    std::vector<QualityActionCandidate> candidates;
    for (const auto& action : active_actions) {
        if (!action.reversible) continue;
        if ((action.temporal_assist || action.domain == QualityDomain::Temporal) && !config_.allow_temporal_assist) continue;
        candidates.push_back(action);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.visual_cost != b.visual_cost) return a.visual_cost > b.visual_cost;
        if (a.expected_ms_gain != b.expected_ms_gain) return a.expected_ms_gain < b.expected_ms_gain;
        return a.id < b.id;
    });

    double spent_ms = 0.0;
    const double budget = std::max(0.0, headroom - config_.restoration_headroom_ms * 0.5);
    for (const auto& candidate : candidates) {
        if (plan.actions.size() >= config_.max_actions_per_plan) break;
        if (spent_ms + candidate.expected_ms_gain > budget) continue;
        plan.actions.push_back(candidate);
        spent_ms += candidate.expected_ms_gain;
        plan.planned_gain_ms += candidate.expected_ms_gain;
        plan.estimated_visual_cost += candidate.visual_cost;
        plan.planned_memory_freed_bytes += candidate.memory_freed_bytes;
    }
    return plan;
}

} // namespace arc
