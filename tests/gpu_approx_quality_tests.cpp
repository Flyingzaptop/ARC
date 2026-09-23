#include "arc/gpu_approx_quality.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace arc;
    GpuApproxTechnical technical{};
    technical.action_supported = technical.structured_shader = technical.finite_bindings = true;
    technical.binding_sealed = technical.effects_complete = true;
    technical.output_coverage = technical.group_barrier_safe = true;
    technical.captured_binding_revision = technical.current_binding_revision = 7;
    GpuApproxQualityEvidence quality{};
    quality.scope = GpuQualityScope::FinalFrameSequence;
    quality.matched_reference = quality.complete_sequence = quality.temporal_reference_matched = true;
    quality.original_variation_measured = true;
    quality.original_ssim = 0.995;
    quality.original_mean_error = 0.001;
    quality.ssim = 0.99;
    quality.mean_error = 0.005;
    quality.tile_p99 = 0.02;
    quality.worst_tile = 0.07;
    quality.temporal_p99 = 0.01;
    quality.captured_surface_revision = quality.current_surface_revision = 2;
    quality.captured_pipeline_generation = quality.current_pipeline_generation = 3;
    const auto limits = optimizer_quality_limits("balanced");
    const auto accepted = admit_gpu_approximation(GpuApproxAction::ComputeDensity, technical, quality, limits);
    assert(accepted.admitted && accepted.quality_verified && accepted.scope == GpuQualityScope::FinalFrameSequence);

    auto bad_technical = technical;
    bad_technical.output_coverage = false;
    assert(admit_gpu_approximation(GpuApproxAction::ComputeDensity, bad_technical, quality, limits).reason ==
           GpuQualityReason::MissingOutputCoverage);
    auto bad_quality = quality;
    bad_quality.tile_p99 = 0.10;
    assert(admit_gpu_approximation(GpuApproxAction::ComputeDensity, technical, bad_quality, limits).reason ==
           GpuQualityReason::QualityThresholdFailed);
    auto moved = quality;
    moved.current_surface_revision++;
    assert(admit_gpu_approximation(GpuApproxAction::ComputeDensity, technical, moved, limits).reason ==
           GpuQualityReason::ContextChanged);
    auto no_temporal = quality;
    no_temporal.complete_sequence = false;
    assert(admit_gpu_approximation(GpuApproxAction::ComputeDensity, technical, no_temporal, limits).reason ==
           GpuQualityReason::TemporalEvidenceMissing);
    auto unproved_precision = technical;
    assert(admit_gpu_approximation(GpuApproxAction::PrecisionReduction, unproved_precision, quality, limits).reason ==
           GpuQualityReason::MissingNumericalModel);
    std::cout << "GPU approximate quality contract PASS\n";
}
