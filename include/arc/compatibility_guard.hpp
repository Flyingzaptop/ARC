#pragma once

#include "arc/resource_graph.hpp"
#include "arc/scene_understanding.hpp"

#include <cstdint>
#include <vector>

namespace arc {

enum class CompatibilityMode : std::uint8_t {
    ObserveOnly,
    QualityOnly,
    FullControl,
};

enum CompatibilityReason : std::uint32_t {
    CompatibilityNone = 0,
    CompatibilityTelemetryErrors = 1u << 0,
    CompatibilityInsufficientCoverage = 1u << 1,
    CompatibilityInsufficientResources = 1u << 2,
    CompatibilityBackendFailures = 1u << 3,
    CompatibilityExternalResidencyUnsafe = 1u << 4,
};

struct CompatibilityTelemetry {
    std::uint64_t observation_failures{};
    std::uint64_t bridge_rejections{};
    std::uint64_t malformed_events{};
    std::uint64_t backend_failures{};
};

struct CompatibilityDecision {
    CompatibilityMode mode{CompatibilityMode::ObserveOnly};
    std::uint32_t reasons{CompatibilityNone};
    double semantic_coverage{};
    bool allow_quality{};
    bool allow_residency{};
};

class CompatibilityGuard final {
public:
    [[nodiscard]] static CompatibilityDecision evaluate(
        const ResourceGraph& graph,
        const std::vector<ResourceSemanticEstimate>& semantics,
        const CompatibilityTelemetry& telemetry) noexcept;

    [[nodiscard]] static bool residency_safe(
        const ResourceRecord& resource,
        const ResourceSemanticEstimate& semantic) noexcept;
};

} // namespace arc
