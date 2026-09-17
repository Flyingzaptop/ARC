#include "arc/action_effect_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>

namespace arc {
namespace {
std::size_t hash_combine(std::size_t a, std::size_t b) noexcept {
    return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
}
}

ActionEffectTracker::ActionEffectTracker(double prior_weight) noexcept
    : prior_weight_(std::max(0.0, prior_weight)) {}

std::size_t ActionEffectTracker::ActionHash::operator()(const ActionEffectKey& key) const noexcept {
    const std::size_t h1 = std::hash<std::uint64_t>{}(key.id);
    const std::size_t h2 = std::hash<std::uint32_t>{}(key.sequence);
    return hash_combine(h1, h2);
}

std::size_t ActionEffectTracker::ContextHash::operator()(const ContextualActionEffectKey& key) const noexcept {
    const std::size_t action = ActionHash{}(key.action);
    const std::size_t context = std::hash<unsigned>{}(static_cast<unsigned>(key.bottleneck));
    return hash_combine(action, context);
}

void ActionEffectTracker::update(RunningState& state, double observed_gain_ms) noexcept {
    if (!std::isfinite(observed_gain_ms)) return;
    ++state.samples;
    if (observed_gain_ms < 0.0) ++state.negative_samples;
    const double delta = observed_gain_ms - state.mean;
    state.mean += delta / static_cast<double>(state.samples);
    const double delta2 = observed_gain_ms - state.mean;
    state.m2 += delta * delta2;
}

ActionEffectStats ActionEffectTracker::stats(const RunningState& state) const noexcept {
    ActionEffectStats out{};
    out.samples = state.samples;
    out.negative_samples = state.negative_samples;
    out.mean_gain_ms = state.mean;
    out.variance_ms2 = state.samples > 1
        ? state.m2 / static_cast<double>(state.samples - 1)
        : 0.0;
    out.negative_fraction = state.samples > 0
        ? static_cast<double>(state.negative_samples) / static_cast<double>(state.samples)
        : 0.0;

    const double sample_factor = static_cast<double>(state.samples) /
        (static_cast<double>(state.samples) + prior_weight_ + 1.0);
    const double noise = std::sqrt(std::max(0.0, out.variance_ms2));
    const double scale = std::max(0.05, std::abs(state.mean));
    const double noise_factor = 1.0 / (1.0 + noise / scale);
    const double sign_factor = 1.0 - 0.75 * std::clamp(out.negative_fraction, 0.0, 1.0);
    out.confidence = std::clamp(sample_factor * noise_factor * sign_factor, 0.0, 1.0);
    return out;
}

QualityActionCandidate ActionEffectTracker::blend(
    const QualityActionCandidate& action,
    const ActionEffectStats& learned,
    double transfer_scale) const noexcept {
    auto out = action;
    const double base_weight = static_cast<double>(learned.samples) /
        (static_cast<double>(learned.samples) + prior_weight_);
    const double learned_weight = std::clamp(base_weight * transfer_scale, 0.0, 1.0);
    out.expected_ms_gain = std::max(0.0,
        action.expected_ms_gain * (1.0 - learned_weight) +
        learned.mean_gain_ms * learned_weight);
    const double reliability = learned.confidence *
        (1.0 - 0.75 * std::clamp(learned.negative_fraction, 0.0, 1.0));
    out.confidence = std::clamp(
        action.confidence * (1.0 - learned_weight) + reliability * learned_weight,
        0.0, 1.0);
    return out;
}

void ActionEffectTracker::record(
    const QualityActionCandidate& action,
    double observed_gain_ms) noexcept {
    if (!std::isfinite(observed_gain_ms)) return;
    update(global_states_[ActionEffectKey{action.id, action.sequence}], observed_gain_ms);
}

void ActionEffectTracker::record(
    const QualityActionCandidate& action,
    BottleneckClass bottleneck,
    double observed_gain_ms) noexcept {
    if (!std::isfinite(observed_gain_ms)) return;
    const ActionEffectKey action_key{action.id, action.sequence};
    update(global_states_[action_key], observed_gain_ms);
    update(contextual_states_[ContextualActionEffectKey{action_key, bottleneck}], observed_gain_ms);
}

std::optional<ActionEffectStats> ActionEffectTracker::find(
    const QualityActionCandidate& action) const noexcept {
    const auto it = global_states_.find(ActionEffectKey{action.id, action.sequence});
    if (it == global_states_.end()) return std::nullopt;
    return stats(it->second);
}

std::optional<ActionEffectStats> ActionEffectTracker::find(
    const QualityActionCandidate& action,
    BottleneckClass bottleneck) const noexcept {
    const auto it = contextual_states_.find(ContextualActionEffectKey{
        ActionEffectKey{action.id, action.sequence}, bottleneck});
    if (it == contextual_states_.end()) return std::nullopt;
    return stats(it->second);
}

QualityActionCandidate ActionEffectTracker::calibrate(
    const QualityActionCandidate& action) const noexcept {
    const auto learned = find(action);
    return learned ? blend(action, *learned, 1.0) : action;
}

QualityActionCandidate ActionEffectTracker::calibrate(
    const QualityActionCandidate& action,
    BottleneckClass bottleneck) const noexcept {
    // Exact context wins.  Cross-context history is only a weak transfer prior;
    // this is the core guard against trusting a globally-good action in the
    // wrong scene.
    if (const auto exact = find(action, bottleneck)) {
        return blend(action, *exact, 1.0);
    }
    if (const auto global = find(action)) {
        return blend(action, *global, 0.25);
    }
    return action;
}

} // namespace arc
