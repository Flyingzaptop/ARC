#pragma once

#include "arc/adaptive_quality.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace arc {

struct ActionEffectKey {
    std::uint64_t id{};
    std::uint32_t sequence{};
    friend bool operator==(const ActionEffectKey&, const ActionEffectKey&) = default;
};

struct ActionEffectStats {
    std::uint64_t samples{};
    double mean_gain_ms{};
    double variance_ms2{};
    double confidence{};
};

class ActionEffectTracker final {
public:
    explicit ActionEffectTracker(double prior_weight = 3.0) noexcept;

    void record(const QualityActionCandidate& action, double observed_gain_ms) noexcept;
    [[nodiscard]] std::optional<ActionEffectStats> find(const QualityActionCandidate& action) const noexcept;
    [[nodiscard]] QualityActionCandidate calibrate(const QualityActionCandidate& action) const noexcept;
    void clear() noexcept { states_.clear(); }

private:
    struct RunningState {
        std::uint64_t samples{};
        double mean{};
        double m2{};
    };
    struct Hash {
        std::size_t operator()(const ActionEffectKey& key) const noexcept;
    };

    std::unordered_map<ActionEffectKey, RunningState, Hash> states_{};
    double prior_weight_{3.0};
};

} // namespace arc
