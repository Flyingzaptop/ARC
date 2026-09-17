#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arc {

enum class QualityDomain : std::uint8_t {
    Texture,
    Bandwidth,
    Raster,
    Geometry,
    Lighting,
    Shadow,
    Temporal,
};

enum class BottleneckClass : std::uint8_t {
    Balanced,
    MemoryCapacity,
    MemoryBandwidth,
    Raster,
    Geometry,
    Lighting,
    UnknownGpu,
    Shadow,
};

struct FrameBudgetSample {
    double frame_ms{};
    double target_frame_ms{16.6667};
    double gpu_busy_fraction{};
    double memory_bandwidth_fraction{};
    double raster_pressure{};
    double geometry_pressure{};
    double lighting_pressure{};
    double shadow_pressure{};
    std::uint64_t local_usage_bytes{};
    std::uint64_t local_budget_bytes{};
};

struct ResourceImportanceSample {
    double screen_coverage{};      // [0,1]
    double visibility{};           // [0,1]
    double motion_salience{};      // [0,1]
    double semantic_importance{0.5}; // UI/face/weapon can be near 1
    double normalized_distance{};  // [0,1], 1 = far
};

struct QualityActionCandidate {
    std::uint64_t id{};
    QualityDomain domain{QualityDomain::Texture};
    std::string label{};
    double expected_ms_gain{};
    double visual_cost{};
    double confidence{1.0};
    std::uint64_t memory_freed_bytes{};
    bool reversible{true};
    bool temporal_assist{};
    std::uint32_t sequence{};
};

struct AdaptiveQualityConfig {
    bool allow_temporal_assist{false};
    double minimum_confidence{0.55};
    double minimum_gain_ms{0.05};
    double visual_cost_floor{0.01};
    double memory_pressure_enter{0.90};
    double memory_pressure_emergency{0.97};
    double restoration_headroom_ms{2.0};
    double memory_value_ms_per_gib{0.40};
    std::uint32_t max_actions_per_plan{8};
};

struct AdaptiveQualityPlan {
    BottleneckClass bottleneck{BottleneckClass::Balanced};
    double frame_deficit_ms{};
    double planned_gain_ms{};
    double estimated_visual_cost{};
    std::uint64_t planned_memory_freed_bytes{};
    bool temporal_used{};
    bool shortfall{};
    std::vector<QualityActionCandidate> actions{};
};

class ResourceImportanceEstimator final {
public:
    [[nodiscard]] static double score(const ResourceImportanceSample& sample) noexcept;
};

class FrameBottleneckAnalyzer final {
public:
    [[nodiscard]] static BottleneckClass classify(const FrameBudgetSample& sample) noexcept;
};

class AdaptiveQualityOptimizer final {
public:
    explicit AdaptiveQualityOptimizer(AdaptiveQualityConfig config = {});

    [[nodiscard]] AdaptiveQualityPlan plan_degrade(
        const FrameBudgetSample& sample,
        const std::vector<QualityActionCandidate>& candidates) const;

    [[nodiscard]] AdaptiveQualityPlan plan_restore(
        const FrameBudgetSample& sample,
        const std::vector<QualityActionCandidate>& active_actions) const;

    [[nodiscard]] const AdaptiveQualityConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] double utility(
        const QualityActionCandidate& candidate,
        double memory_pressure) const noexcept;
    [[nodiscard]] bool domain_matches(
        BottleneckClass bottleneck,
        QualityDomain domain) const noexcept;

    AdaptiveQualityConfig config_{};
};

} // namespace arc
