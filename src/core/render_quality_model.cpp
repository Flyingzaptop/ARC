#include "arc/render_quality_model.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

double safe_nonnegative(double v) noexcept {
    return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}

double normalized_pressure(double domain_ms, double total_ms) noexcept {
    if (!(total_ms > 0.0)) return 0.0;
    // Five similarly expensive passes land near 0.5. A genuinely dominant
    // pass rises above the UnknownGpu prior and becomes the classified bottleneck.
    return std::clamp((safe_nonnegative(domain_ms) / total_ms) * 2.5, 0.0, 1.0);
}

} // namespace

double RenderQualityModel::domain_time_ms(
    const RenderPassTimings& timings,
    QualityDomain domain) noexcept {
    switch (domain) {
    case QualityDomain::Texture:
    case QualityDomain::Bandwidth:
        return safe_nonnegative(timings.texture_ms);
    case QualityDomain::Geometry:
        return safe_nonnegative(timings.geometry_ms);
    case QualityDomain::Raster:
        return safe_nonnegative(timings.raster_ms);
    case QualityDomain::Lighting:
        return safe_nonnegative(timings.lighting_ms);
    case QualityDomain::Shadow:
        return safe_nonnegative(timings.shadow_ms);
    case QualityDomain::Temporal:
        return safe_nonnegative(timings.total_ms);
    }
    return 0.0;
}

FrameBudgetSample RenderQualityModel::make_frame_sample(
    const RenderPassTimings& timings,
    double target_frame_ms,
    std::uint64_t local_usage_bytes,
    std::uint64_t local_budget_bytes,
    double gpu_busy_fraction) noexcept {
    const double total = safe_nonnegative(timings.total_ms);
    FrameBudgetSample out{};
    out.frame_ms = total;
    out.target_frame_ms = std::max(0.1, safe_nonnegative(target_frame_ms));
    out.gpu_busy_fraction = std::clamp(gpu_busy_fraction, 0.0, 1.0);
    out.memory_bandwidth_fraction = normalized_pressure(timings.texture_ms, total);
    out.raster_pressure = normalized_pressure(timings.raster_ms, total);
    out.geometry_pressure = normalized_pressure(timings.geometry_ms, total);
    out.lighting_pressure = normalized_pressure(timings.lighting_ms, total);
    out.local_usage_bytes = local_usage_bytes;
    out.local_budget_bytes = local_budget_bytes;
    return out;
}

std::vector<QualityActionCandidate> RenderQualityModel::build_candidates(
    const RenderPassTimings& timings,
    const std::vector<RenderQualityStep>& steps) {
    std::vector<QualityActionCandidate> out;
    out.reserve(steps.size());
    for (const auto& step : steps) {
        const double domain_ms = domain_time_ms(timings, step.domain);
        const double reduction = std::clamp(step.relative_cost_reduction, 0.0, 1.0);
        QualityActionCandidate candidate{};
        candidate.id = step.id;
        candidate.domain = step.domain;
        candidate.label = step.label;
        candidate.expected_ms_gain = domain_ms * reduction;
        candidate.visual_cost = std::max(0.0, step.visual_cost);
        candidate.confidence = std::clamp(step.confidence, 0.0, 1.0);
        candidate.memory_freed_bytes = step.memory_freed_bytes;
        candidate.reversible = step.reversible;
        candidate.temporal_assist = step.temporal_assist;
        candidate.sequence = step.sequence;
        out.push_back(std::move(candidate));
    }
    return out;
}

} // namespace arc
