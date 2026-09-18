#include "arc/compatibility_guard.hpp"

#include <algorithm>

namespace arc {

bool CompatibilityGuard::residency_safe(
    const ResourceRecord& resource,
    const ResourceSemanticEstimate& semantic) noexcept {
    if (!resource.alive) return false;
    if (resource.description.allocation_kind == ResourceAllocationKind::External ||
        resource.description.allocation_kind == ResourceAllocationKind::Reserved) {
        return false;
    }
    if (resource.safety == SafetyClass::Red ||
        resource.safety == SafetyClass::Unknown) {
        return false;
    }
    if (semantic.confidence < 0.60) return false;

    switch (semantic.semantic) {
    case ResourceSemantic::MaterialTexture:
        return resource.safety == SafetyClass::GreenCandidate;
    case ResourceSemantic::GeometryBuffer:
        return resource.safety == SafetyClass::GreenCandidate &&
            resource.write_count == 0;
    default:
        return false;
    }
}

CompatibilityDecision CompatibilityGuard::evaluate(
    const ResourceGraph& graph,
    const std::vector<ResourceSemanticEstimate>& semantics,
    const CompatibilityTelemetry& telemetry) noexcept {
    CompatibilityDecision out{};
    out.semantic_coverage =
        SceneUnderstandingModel::semantic_coverage(semantics, 0.55);

    if (telemetry.observation_failures ||
        telemetry.bridge_rejections ||
        telemetry.malformed_events ||
        graph.errors()) {
        out.reasons |= CompatibilityTelemetryErrors;
    }
    if (telemetry.backend_failures >= 3) {
        out.reasons |= CompatibilityBackendFailures;
    }
    if (graph.resource_count() < 16) {
        out.reasons |= CompatibilityInsufficientResources;
    }
    if (out.semantic_coverage < 0.20) {
        out.reasons |= CompatibilityInsufficientCoverage;
    }

    std::size_t residency_candidates = 0;
    std::size_t external = 0;
    for (const auto& estimate : semantics) {
        const auto record = graph.find(estimate.resource);
        if (!record) continue;
        if (record->description.allocation_kind == ResourceAllocationKind::External)
            ++external;
        if (residency_safe(*record, estimate)) ++residency_candidates;
    }

    if (external > 0 && residency_candidates == 0)
        out.reasons |= CompatibilityExternalResidencyUnsafe;

    const bool telemetry_clean =
        (out.reasons & (CompatibilityTelemetryErrors | CompatibilityBackendFailures)) == 0;
    const bool enough_evidence =
        (out.reasons & (CompatibilityInsufficientResources | CompatibilityInsufficientCoverage)) == 0;

    out.allow_quality = telemetry_clean && enough_evidence;
    out.allow_residency = out.allow_quality && residency_candidates > 0;

    if (out.allow_quality && out.allow_residency)
        out.mode = CompatibilityMode::FullControl;
    else if (out.allow_quality)
        out.mode = CompatibilityMode::QualityOnly;
    else
        out.mode = CompatibilityMode::ObserveOnly;

    return out;
}

} // namespace arc
