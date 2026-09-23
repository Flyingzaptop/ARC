#pragma once

#include "arc/optimizer_quality_profile.hpp"
#include <cmath>
#include <cstdint>
#include <string_view>

namespace arc {

enum class GpuApproxAction : std::uint8_t {
    IndependentSample, ComparisonTap, MipSelection, ComputeDensity,
    SpatialVrs, StableInputSpecialization, PrecisionReduction, RayReduction
};
enum class GpuQualityScope : std::uint8_t { None, IsolatedScratch, FinalFrameSequence };
enum class GpuQualityReason : std::uint8_t {
    Accepted, UnsupportedAction, MissingTechnicalProof, UnknownEffects,
    MissingOutputCoverage, UnknownResourceMeaning, MissingNumericalModel,
    BindingChanged, ContextChanged, ReferenceUnmatched,
    OriginalVariationUnmeasured, QualityThresholdFailed, TemporalEvidenceMissing
};
inline constexpr std::string_view gpu_quality_reason(GpuQualityReason reason) noexcept {
    switch (reason) {
    case GpuQualityReason::Accepted: return "accepted_scoped_quality";
    case GpuQualityReason::UnsupportedAction: return "unsupported_action";
    case GpuQualityReason::MissingTechnicalProof: return "technical_proof_missing";
    case GpuQualityReason::UnknownEffects: return "unknown_shader_effects";
    case GpuQualityReason::MissingOutputCoverage: return "output_coverage_or_group_semantics_unknown";
    case GpuQualityReason::UnknownResourceMeaning: return "resource_meaning_unknown";
    case GpuQualityReason::MissingNumericalModel: return "numerical_or_input_model_missing";
    case GpuQualityReason::BindingChanged: return "binding_generation_changed";
    case GpuQualityReason::ContextChanged: return "quality_context_changed";
    case GpuQualityReason::ReferenceUnmatched: return "quality_reference_unmatched";
    case GpuQualityReason::OriginalVariationUnmeasured: return "original_original_variation_unmeasured";
    case GpuQualityReason::QualityThresholdFailed: return "quality_threshold_failed";
    case GpuQualityReason::TemporalEvidenceMissing: return "temporal_sequence_missing";
    }
    return "unknown_quality_reason";
}

// Populated from structured shader/emitter/resource admission, never from a
// pipeline name or the existence of a proposed variant alone.
struct GpuApproxTechnical {
    bool action_supported{};
    bool structured_shader{};
    bool finite_bindings{};
    bool binding_sealed{};
    bool effects_complete{};
    bool output_coverage{};
    bool group_barrier_safe{};
    bool resource_meaning_known{};
    bool numerical_model{};
    std::uint64_t captured_binding_revision{}, current_binding_revision{};
};

// Final-frame evidence comes from the existing five-frame original/candidate
// critic. Its original/original reference check measures scene variation and
// is never subtracted from the candidate's error thresholds.
struct GpuApproxQualityEvidence {
    GpuQualityScope scope{GpuQualityScope::None};
    bool matched_reference{}, complete_sequence{}, temporal_reference_matched{};
    bool original_variation_measured{};
    double original_ssim{}, original_mean_error{};
    double ssim{}, mean_error{}, tile_p99{}, worst_tile{}, temporal_p99{};
    std::uint64_t captured_surface_revision{}, current_surface_revision{};
    std::uint64_t captured_pipeline_generation{}, current_pipeline_generation{};
};
struct GpuApproxQualityDecision {
    bool admitted{};
    GpuQualityReason reason{GpuQualityReason::UnsupportedAction};
    GpuQualityScope scope{GpuQualityScope::None};
    bool quality_verified{};
};

inline GpuApproxQualityDecision admit_gpu_approximation(
    GpuApproxAction action, const GpuApproxTechnical& technical,
    const GpuApproxQualityEvidence& quality, const OptimizerQualityLimits& limits) noexcept {
    const auto decline = [&](GpuQualityReason reason) {
        return GpuApproxQualityDecision{false, reason, quality.scope, false};
    };
    if (!technical.action_supported) return decline(GpuQualityReason::UnsupportedAction);
    if (!technical.structured_shader || !technical.finite_bindings || !technical.binding_sealed)
        return decline(GpuQualityReason::MissingTechnicalProof);
    if (!technical.effects_complete) return decline(GpuQualityReason::UnknownEffects);
    if (technical.captured_binding_revision != technical.current_binding_revision)
        return decline(GpuQualityReason::BindingChanged);
    if (action == GpuApproxAction::ComputeDensity &&
        (!technical.output_coverage || !technical.group_barrier_safe))
        return decline(GpuQualityReason::MissingOutputCoverage);
    if (action == GpuApproxAction::SpatialVrs && !technical.resource_meaning_known)
        return decline(GpuQualityReason::UnknownResourceMeaning);
    if ((action == GpuApproxAction::StableInputSpecialization ||
         action == GpuApproxAction::PrecisionReduction) && !technical.numerical_model)
        return decline(GpuQualityReason::MissingNumericalModel);
    if (action == GpuApproxAction::RayReduction && !technical.resource_meaning_known)
        return decline(GpuQualityReason::UnknownResourceMeaning);
    if (quality.captured_surface_revision != quality.current_surface_revision ||
        quality.captured_pipeline_generation != quality.current_pipeline_generation)
        return decline(GpuQualityReason::ContextChanged);
    if (quality.scope != GpuQualityScope::FinalFrameSequence || !quality.matched_reference)
        return decline(GpuQualityReason::ReferenceUnmatched);
    if (!quality.original_variation_measured || !std::isfinite(quality.original_ssim) ||
        !std::isfinite(quality.original_mean_error))
        return decline(GpuQualityReason::OriginalVariationUnmeasured);
    // These are independent reference-confidence limits, not an allowance
    // added to the candidate limits.
    if (quality.original_ssim < .99 || quality.original_mean_error > .002)
        return decline(GpuQualityReason::ReferenceUnmatched);
    if (!std::isfinite(quality.ssim) || !std::isfinite(quality.mean_error) ||
        !std::isfinite(quality.tile_p99) || !std::isfinite(quality.worst_tile) ||
        quality.ssim < limits.ssim || quality.mean_error > limits.mean ||
        quality.tile_p99 > limits.p99 || quality.worst_tile > limits.worst)
        return decline(GpuQualityReason::QualityThresholdFailed);
    if (!quality.complete_sequence || !quality.temporal_reference_matched ||
        !std::isfinite(quality.temporal_p99))
        return decline(GpuQualityReason::TemporalEvidenceMissing);
    if (quality.temporal_p99 > limits.temporal)
        return decline(GpuQualityReason::QualityThresholdFailed);
    return {true, GpuQualityReason::Accepted, quality.scope, true};
}

} // namespace arc
