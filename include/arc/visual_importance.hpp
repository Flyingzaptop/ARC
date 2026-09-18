#pragma once

#include "arc/temporal_visibility.hpp"

#include <optional>

namespace arc {

struct VisualImportanceConfig {
    // Unknown evidence reserves importance instead of making uncertain work look cheap.
    double uncertainty_reserve{0.20};
    double peripheral_floor{0.70};
    double future_2f_weight{1.00};
    double future_8f_weight{0.90};
    double future_30f_weight{0.65};
};

struct VisualImportanceHint {
    // Normalized screen coordinates in [-1,1], where (0,0) is center.
    // Missing coordinates are treated conservatively as central.
    std::optional<double> screen_x{};
    std::optional<double> screen_y{};

    // Optional backend-neutral perceptual sensitivity estimate. Unknown defaults
    // to 1.0, never to a cheap value. This is not an engine semantic label.
    std::optional<double> perceptual_sensitivity{};

    // Optional confidence that the tracked contribution actually reaches the
    // presented composition. Unknown defaults to 1.0 conservatively.
    std::optional<double> composition_relevance{};
};

struct VisualImportance {
    VisualTrackId id{};
    double score{};

    double coverage_component{};
    double visibility_component{};
    double temporal_component{};
    double centrality_component{};
    double sensitivity_component{};
    double composition_component{};
    double uncertainty_component{};

    double confidence{};
    bool conservative_unknowns{};
};

class VisualImportanceModel final {
public:
    explicit VisualImportanceModel(VisualImportanceConfig config = {});

    [[nodiscard]] VisualImportance evaluate(
        const TemporalVisibilityState& visibility,
        const VisualImportanceHint& hint = {}) const noexcept;

    [[nodiscard]] const VisualImportanceConfig& config() const noexcept { return config_; }

private:
    VisualImportanceConfig config_{};
};

}  // namespace arc
