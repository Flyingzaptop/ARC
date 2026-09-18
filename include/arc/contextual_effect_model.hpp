#pragma once

#include "arc/adaptive_quality.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <cstddef>
#include <unordered_map>

namespace arc {

struct ContextualEffectPrediction {
    std::uint64_t samples{};
    double predicted_ms{};
    double residual_sigma_ms{};
    double confidence{};
};

class ContextualActionEffectTracker final {
public:
    explicit ContextualActionEffectTracker(double prior_weight = 6.0) noexcept;

    void record(
        const QualityActionCandidate& action,
        const FrameBudgetSample& context,
        double observed_ms) noexcept;

    [[nodiscard]] std::optional<ContextualEffectPrediction> predict(
        const QualityActionCandidate& action,
        const FrameBudgetSample& context) const noexcept;

    [[nodiscard]] QualityActionCandidate calibrate(
        const QualityActionCandidate& action,
        const FrameBudgetSample& context) const noexcept;

    void clear() noexcept { states_.clear(); }
    [[nodiscard]] std::size_t model_count() const noexcept { return states_.size(); }
    [[nodiscard]] std::uint64_t total_samples() const noexcept {
        std::uint64_t total = 0;
        for (const auto& [_, state] : states_) total += state.samples;
        return total;
    }

private:
    struct Key {
        std::uint64_t id{};
        std::uint32_t sequence{};
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct State {
        std::array<double, 8> weights{};
        std::uint64_t samples{};
        double residual_mean{};
        double residual_m2{};
    };

    [[nodiscard]] static std::array<double, 8> features(
        const FrameBudgetSample& context) noexcept;
    [[nodiscard]] static double dot(
        const std::array<double, 8>& a,
        const std::array<double, 8>& b) noexcept;

    std::unordered_map<Key, State, Hash> states_{};
    double prior_weight_{6.0};
};

} // namespace arc
