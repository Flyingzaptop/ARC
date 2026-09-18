#pragma once

#include "arc/adaptive_quality.hpp"
#include "arc/resource_graph.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace arc {

enum class ResourceSemantic : std::uint8_t {
    Unknown,
    MaterialTexture,
    ShadowMap,
    DepthTarget,
    ColorTarget,
    TransientTarget,
    StorageTexture,
    HistoryTexture,
    GeometryBuffer,
    ConstantBuffer,
};

struct ResourceSemanticEstimate {
    ResourceId resource{};
    ResourceSemantic semantic{ResourceSemantic::Unknown};
    double confidence{};
    std::array<double, 10> scores{};
};

enum class WorkloadClass : std::uint8_t {
    Unknown,
    Balanced,
    RasterHeavy,
    GeometryHeavy,
    ComputeHeavy,
    BandwidthHeavy,
    ShadowHeavy,
    Mixed,
};

struct WorkloadTelemetrySample {
    double gpu_ms{};
    double target_ms{};
    std::uint64_t frames{1};
    std::uint64_t draws{};
    std::uint64_t indexed_draws{};
    std::uint64_t dispatches{};
    std::uint64_t indirect{};
    std::uint64_t resource_uses{};
    std::uint64_t descriptor_writes{};
    std::uint64_t barriers{};
    std::uint64_t copies{};
};

struct WorkloadSignature {
    WorkloadClass dominant{WorkloadClass::Unknown};
    double confidence{};
    double raster{};
    double geometry{};
    double compute{};
    double bandwidth{};
    double shadow{};
    double activity{};
};

class SceneUnderstandingModel final {
public:
    [[nodiscard]] static ResourceSemanticEstimate infer_resource(
        const ResourceRecord& resource,
        FrameId current_frame) noexcept;

    [[nodiscard]] static std::vector<ResourceSemanticEstimate> infer_all(
        const ResourceGraph& graph);

    [[nodiscard]] static double semantic_coverage(
        const std::vector<ResourceSemanticEstimate>& estimates,
        double minimum_confidence = 0.55) noexcept;

    [[nodiscard]] static WorkloadSignature infer_workload(
        const WorkloadTelemetrySample& sample) noexcept;

    [[nodiscard]] static FrameBudgetSample enrich_frame(
        FrameBudgetSample frame,
        const WorkloadSignature& signature) noexcept;
};

} // namespace arc
