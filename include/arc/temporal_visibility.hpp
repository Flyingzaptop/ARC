#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace arc {

using VisualTrackId = std::uint64_t;

enum class VisibilityPhase : std::uint8_t {
    Unknown,
    Hidden,
    Visible,
    Entering,
    Leaving,
};

struct TemporalVisibilityConfig {
    double coverage_alpha{0.45};
    double velocity_alpha{0.35};
    double missing_decay{0.82};
    double confidence_decay{0.94};
    double entering_velocity{0.0010};
    double leaving_velocity{-0.0010};
    double hidden_coverage{0.00005};
    double visible_coverage{0.00020};
    std::uint32_t stale_frames{300};
    std::size_t max_tracks{4096};
};

struct VisibilityObservation {
    VisualTrackId id{};
    std::uint64_t frame{};

    // Conservative local-target bound from Stage 17 when available.
    std::optional<double> local_coverage_upper{};

    // Stronger visibility evidence, for example from an occlusion/sample path.
    // This is visible final/local coverage in normalized [0,1] units, not a label.
    std::optional<double> visible_coverage{};

    // False is positive evidence that this work cannot contribute to the
    // current presented image. nullopt means unknown.
    std::optional<bool> present_reachable{};

    // Confidence of the strongest observation supplied by the caller.
    double confidence{0.5};
};

struct TemporalVisibilityState {
    VisualTrackId id{};
    std::uint64_t frame{};
    VisibilityPhase phase{VisibilityPhase::Unknown};

    double estimated_visible_coverage{};
    double coverage_upper{};
    double coverage_velocity{};

    double predicted_2f{};
    double predicted_8f{};
    double predicted_30f{};

    // A bounded summary used by Stage 19. It includes short-horizon prediction
    // and recent visibility, but does not include GPU cost.
    double temporal_relevance{};

    double confidence{};
    std::uint32_t frames_since_observed{};
    std::uint32_t frames_since_visible{};
    bool direct_visibility_sample{};
    bool present_reachable_known{};
};

class TemporalVisibilityModel final {
public:
    explicit TemporalVisibilityModel(TemporalVisibilityConfig config = {});

    // Advance all tracks to a new monotonically increasing frame. Missing
    // observations decay conservatively rather than becoming an immediate zero.
    bool advance(std::uint64_t frame);

    // Observe one stable visual track for the current frame.
    bool observe(const VisibilityObservation& observation);

    [[nodiscard]] const TemporalVisibilityState* find(VisualTrackId id) const noexcept;
    [[nodiscard]] std::vector<TemporalVisibilityState> snapshots() const;
    [[nodiscard]] std::size_t track_count() const noexcept { return tracks_.size(); }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }

    void clear() noexcept;

private:
    struct Track {
        TemporalVisibilityState state{};
        std::uint64_t last_observed_frame{};
        std::uint64_t last_visible_frame{};
        double smoothed_coverage{};
        double smoothed_velocity{};
    };

    TemporalVisibilityConfig config_{};
    std::unordered_map<VisualTrackId, Track> tracks_{};
    std::uint64_t frame_{};

    [[nodiscard]] static bool probability(double value) noexcept;
    [[nodiscard]] double predict(double coverage, double velocity, double frames) const noexcept;
    void refresh(Track& track) noexcept;
};

}  // namespace arc
