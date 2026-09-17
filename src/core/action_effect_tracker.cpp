#include "arc/action_effect_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>

namespace arc {

ActionEffectTracker::ActionEffectTracker(double prior_weight) noexcept
    : prior_weight_(std::max(0.0, prior_weight)) {}

std::size_t ActionEffectTracker::Hash::operator()(const ActionEffectKey& key) const noexcept {
    const std::size_t h1 = std::hash<std::uint64_t>{}(key.id);
    const std::size_t h2 = std::hash<std::uint32_t>{}(key.sequence);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
}

void ActionEffectTracker::record(const QualityActionCandidate& action, double observed_gain_ms) noexcept {
    if (!std::isfinite(observed_gain_ms)) return;
    auto& state = states_[ActionEffectKey{action.id, action.sequence}];
    ++state.samples;
    const double delta = observed_gain_ms - state.mean;
    state.mean += delta / static_cast<double>(state.samples);
    const double delta2 = observed_gain_ms - state.mean;
    state.m2 += delta * delta2;
}

std::optional<ActionEffectStats> ActionEffectTracker::find(const QualityActionCandidate& action) const noexcept {
    const auto it = states_.find(ActionEffectKey{action.id, action.sequence});
    if (it == states_.end()) return std::nullopt;
    const auto& state = it->second;
    ActionEffectStats out{};
    out.samples = state.samples;
    out.mean_gain_ms = state.mean;
    out.variance_ms2 = state.samples > 1 ? state.m2 / static_cast<double>(state.samples - 1) : 0.0;
    const double sample_factor = static_cast<double>(state.samples) / (static_cast<double>(state.samples) + prior_weight_ + 1.0);
    const double noise = std::sqrt(std::max(0.0, out.variance_ms2));
    const double noise_factor = 1.0 / (1.0 + noise / std::max(0.05, std::abs(state.mean)));
    out.confidence = std::clamp(sample_factor * noise_factor, 0.0, 1.0);
    return out;
}

QualityActionCandidate ActionEffectTracker::calibrate(const QualityActionCandidate& action) const noexcept {
    auto out = action;
    const auto stats = find(action);
    if (!stats) return out;

    const double learned_weight = static_cast<double>(stats->samples) /
        (static_cast<double>(stats->samples) + prior_weight_);
    out.expected_ms_gain = std::max(0.0,
        action.expected_ms_gain * (1.0 - learned_weight) + stats->mean_gain_ms * learned_weight);
    out.confidence = std::clamp(
        action.confidence * (1.0 - learned_weight) + stats->confidence * learned_weight,
        0.0, 1.0);
    return out;
}

} // namespace arc
