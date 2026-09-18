#include "arc/contextual_effect_model.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace arc {

ContextualActionEffectTracker::ContextualActionEffectTracker(
    const double prior_weight) noexcept
    : prior_weight_(std::max(0.0, prior_weight)) {}

std::size_t ContextualActionEffectTracker::Hash::operator()(const Key& key) const noexcept {
    const std::size_t h1 = std::hash<std::uint64_t>{}(key.id);
    const std::size_t h2 = std::hash<std::uint32_t>{}(key.sequence);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
}

std::array<double, 8> ContextualActionEffectTracker::features(
    const FrameBudgetSample& c) noexcept {
    const double memory_pressure = c.local_budget_bytes
        ? std::clamp(
            static_cast<double>(c.local_usage_bytes) /
                static_cast<double>(c.local_budget_bytes),
            0.0, 1.5)
        : 0.0;
    return {
        1.0,
        std::clamp(c.gpu_busy_fraction, 0.0, 1.0),
        std::clamp(c.memory_bandwidth_fraction, 0.0, 1.0),
        std::clamp(c.raster_pressure, 0.0, 1.0),
        std::clamp(c.geometry_pressure, 0.0, 1.0),
        std::clamp(c.lighting_pressure, 0.0, 1.0),
        std::clamp(c.shadow_pressure, 0.0, 1.0),
        std::clamp(memory_pressure, 0.0, 1.0),
    };
}

double ContextualActionEffectTracker::dot(
    const std::array<double, 8>& a,
    const std::array<double, 8>& b) noexcept {
    double out = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) out += a[i] * b[i];
    return out;
}

void ContextualActionEffectTracker::record(
    const QualityActionCandidate& action,
    const FrameBudgetSample& context,
    const double observed_ms) noexcept {
    if (!std::isfinite(observed_ms)) return;

    auto& state = states_[Key{action.id, action.sequence}];
    const auto x = features(context);
    const double prediction = dot(state.weights, x);
    const double error = observed_ms - prediction;

    const double norm = std::max(1.0, dot(x, x));
    const double learning_rate =
        0.22 / std::sqrt(1.0 + static_cast<double>(state.samples) * 0.20);
    for (std::size_t i = 0; i < state.weights.size(); ++i) {
        state.weights[i] = std::clamp(
            state.weights[i] + learning_rate * error * x[i] / norm,
            -20.0, 20.0);
    }

    ++state.samples;
    const double residual = observed_ms - dot(state.weights, x);
    const double delta = residual - state.residual_mean;
    state.residual_mean += delta / static_cast<double>(state.samples);
    const double delta2 = residual - state.residual_mean;
    state.residual_m2 += delta * delta2;
}

std::optional<ContextualEffectPrediction> ContextualActionEffectTracker::predict(
    const QualityActionCandidate& action,
    const FrameBudgetSample& context) const noexcept {
    const auto it = states_.find(Key{action.id, action.sequence});
    if (it == states_.end() || it->second.samples == 0) return std::nullopt;

    const auto& state = it->second;
    ContextualEffectPrediction out{};
    out.samples = state.samples;
    out.predicted_ms = std::max(0.0, dot(state.weights, features(context)));
    const double variance = state.samples > 1
        ? state.residual_m2 / static_cast<double>(state.samples - 1)
        : 0.0;
    out.residual_sigma_ms = std::sqrt(std::max(0.0, variance));

    const double sample_factor =
        static_cast<double>(state.samples) /
        (static_cast<double>(state.samples) + prior_weight_ + 1.0);
    const double noise_factor =
        1.0 / (1.0 + out.residual_sigma_ms /
            std::max(0.05, std::abs(out.predicted_ms)));
    out.confidence = std::clamp(sample_factor * noise_factor, 0.0, 1.0);
    return out;
}

QualityActionCandidate ContextualActionEffectTracker::calibrate(
    const QualityActionCandidate& action,
    const FrameBudgetSample& context) const noexcept {
    auto out = action;
    const auto learned = predict(action, context);
    if (!learned) return out;

    const double weight =
        static_cast<double>(learned->samples) /
        (static_cast<double>(learned->samples) + prior_weight_);
    out.expected_ms_gain = std::max(
        0.0,
        action.expected_ms_gain * (1.0 - weight) +
            learned->predicted_ms * weight);
    out.confidence = std::clamp(
        action.confidence * (1.0 - weight) +
            learned->confidence * weight,
        0.0, 1.0);
    return out;
}

} // namespace arc
