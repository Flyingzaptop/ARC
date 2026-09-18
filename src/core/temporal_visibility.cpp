#include "arc/temporal_visibility.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace arc {
namespace {

double clamp01(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

std::uint32_t saturating_frames(std::uint64_t value) noexcept {
    return value > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(value);
}

}  // namespace

VisualTrackFingerprint make_visual_track_fingerprint(const WorkObservation& work) noexcept {
    std::vector<std::uint64_t> identity;
    identity.reserve(work.accesses.size());
    bool have_read = false;
    bool have_write = false;
    bool observed_access = false;

    for (const auto& access : work.accesses) {
        const std::uint64_t resource = static_cast<std::uint64_t>(access.resource);
        if (!resource) continue;
        const std::uint64_t encoded =
            (resource << 3U) ^
            (access.write ? 0x4ULL : 0x0ULL) ^
            (access.full_overwrite ? 0x2ULL : 0x0ULL) ^
            (access.evidence == AccessEvidence::Observed ? 0x1ULL : 0x0ULL);
        identity.push_back(encoded);
        have_write = have_write || access.write;
        have_read = have_read || !access.write;
        observed_access = observed_access || access.evidence == AccessEvidence::Observed;
    }

    std::ranges::sort(identity);
    identity.erase(std::unique(identity.begin(), identity.end()), identity.end());

    if (!work.pipeline && identity.empty() && !work.items) return {};

    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
        hash ^= hash >> 32U;
    };

    mix(static_cast<std::uint64_t>(work.kind) + 1ULL);
    mix(work.pipeline);
    mix(work.items);
    mix(work.copy_bytes);
    mix(static_cast<std::uint64_t>(work.raster.width));
    mix(static_cast<std::uint64_t>(work.raster.height));
    for (const auto value : identity) mix(value);
    if (!hash) hash = 1;

    double confidence = 0.20;
    if (work.pipeline) confidence += 0.20;
    if (work.items || work.copy_bytes) confidence += 0.10;
    if (have_read) confidence += 0.15;
    if (have_write) confidence += 0.15;
    if (observed_access) confidence += 0.05;
    if (work.raster.known) confidence += 0.05;
    if (work.bindings_complete) confidence += 0.05;

    return {hash, clamp01(confidence)};
}

std::optional<double> visible_coverage_from_occlusion(
    std::uint64_t passed_samples,
    std::uint64_t target_pixels,
    std::uint32_t sample_count) noexcept {
    if (!target_pixels || !sample_count) return {};
    const long double denominator =
        static_cast<long double>(target_pixels) * static_cast<long double>(sample_count);
    if (!(denominator > 0.0L) || !std::isfinite(static_cast<double>(denominator))) return {};
    const long double value = static_cast<long double>(passed_samples) / denominator;
    if (!std::isfinite(static_cast<double>(value))) return {};
    return clamp01(static_cast<double>(value));
}

VisibilityObservation make_potential_visibility_observation(
    const AttributionNode& node,
    std::uint64_t frame,
    bool present_reachable) noexcept {
    VisibilityObservation observation{};
    const auto fingerprint = make_visual_track_fingerprint(node.work);
    observation.id = fingerprint.id;
    observation.frame = frame;
    observation.local_coverage_upper = node.local_coverage_upper;
    observation.present_reachable = present_reachable;

    double confidence = fingerprint.confidence;
    confidence *= node.unresolved_inputs ? 0.55 : 0.80;
    confidence *= node.local_coverage_upper ? 0.80 : 0.40;
    observation.confidence = clamp01(confidence);
    return observation;
}

TemporalVisibilityModel::TemporalVisibilityModel(TemporalVisibilityConfig config)
    : config_(config) {
    const bool valid =
        probability(config_.coverage_alpha) &&
        probability(config_.velocity_alpha) &&
        probability(config_.missing_decay) &&
        probability(config_.confidence_decay) &&
        std::isfinite(config_.entering_velocity) &&
        std::isfinite(config_.leaving_velocity) &&
        config_.entering_velocity > 0.0 &&
        config_.leaving_velocity < 0.0 &&
        probability(config_.hidden_coverage) &&
        probability(config_.visible_coverage) &&
        config_.hidden_coverage <= config_.visible_coverage &&
        config_.stale_frames > 0 &&
        config_.max_tracks > 0;
    if (!valid) config_ = TemporalVisibilityConfig{};
}

bool TemporalVisibilityModel::probability(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

double TemporalVisibilityModel::predict(double coverage, double velocity, double frames) const noexcept {
    return clamp01(coverage + velocity * frames);
}

void TemporalVisibilityModel::refresh(Track& track) noexcept {
    auto& state = track.state;
    state.estimated_visible_coverage = clamp01(track.smoothed_coverage);
    state.coverage_velocity = std::isfinite(track.smoothed_velocity) ? track.smoothed_velocity : 0.0;
    state.predicted_2f = predict(state.estimated_visible_coverage, state.coverage_velocity, 2.0);
    state.predicted_8f = predict(state.estimated_visible_coverage, state.coverage_velocity, 8.0);
    state.predicted_30f = predict(state.estimated_visible_coverage, state.coverage_velocity, 30.0);

    const double future = std::max({
        state.estimated_visible_coverage,
        state.predicted_2f,
        0.90 * state.predicted_8f,
        0.65 * state.predicted_30f
    });
    const double recent = state.frames_since_visible == (std::numeric_limits<std::uint32_t>::max)()
        ? 0.0
        : std::exp(-static_cast<double>(state.frames_since_visible) / 18.0);
    state.temporal_relevance = clamp01(std::max(future, state.estimated_visible_coverage * (0.65 + 0.35 * recent)));

    if (state.frames_since_observed > 0) {
        if (state.estimated_visible_coverage <= config_.hidden_coverage && state.frames_since_visible > 2) {
            state.phase = VisibilityPhase::Hidden;
        } else if (state.coverage_velocity > config_.entering_velocity) {
            state.phase = VisibilityPhase::Entering;
        } else if (state.coverage_velocity < config_.leaving_velocity) {
            state.phase = VisibilityPhase::Leaving;
        } else if (state.estimated_visible_coverage >= config_.visible_coverage) {
            state.phase = VisibilityPhase::Visible;
        } else {
            state.phase = VisibilityPhase::Unknown;
        }
        return;
    }

    if (state.estimated_visible_coverage <= config_.hidden_coverage) {
        state.phase = state.present_reachable_known ? VisibilityPhase::Hidden : VisibilityPhase::Unknown;
    } else if (state.coverage_velocity > config_.entering_velocity) {
        state.phase = VisibilityPhase::Entering;
    } else if (state.coverage_velocity < config_.leaving_velocity) {
        state.phase = VisibilityPhase::Leaving;
    } else if (state.estimated_visible_coverage >= config_.visible_coverage) {
        state.phase = VisibilityPhase::Visible;
    } else {
        state.phase = VisibilityPhase::Unknown;
    }
}

bool TemporalVisibilityModel::advance(std::uint64_t frame) {
    if (!frame || frame < frame_) return false;
    if (frame == frame_) return true;

    frame_ = frame;
    const std::uint64_t decay_through = frame_ > 0 ? frame_ - 1 : 0;

    for (auto it = tracks_.begin(); it != tracks_.end();) {
        auto& track = it->second;
        const auto since_observed = track.last_observed_frame ? frame_ - track.last_observed_frame : frame_;
        if (track.last_observed_frame && since_observed > config_.stale_frames) {
            it = tracks_.erase(it);
            continue;
        }

        if (track.last_state_frame < decay_through) {
            const auto steps = decay_through - track.last_state_frame;
            const double decay_steps = static_cast<double>(steps);
            track.smoothed_coverage *= std::pow(config_.missing_decay, decay_steps);
            track.smoothed_velocity *= std::pow(config_.missing_decay, decay_steps);
            track.state.confidence = clamp01(
                track.state.confidence * std::pow(config_.confidence_decay, decay_steps));
            track.last_state_frame = decay_through;
        }

        track.state.frame = frame_;
        track.state.frames_since_observed = saturating_frames(since_observed);
        track.state.frames_since_visible = track.last_visible_frame
            ? saturating_frames(frame_ - track.last_visible_frame)
            : (std::numeric_limits<std::uint32_t>::max)();
        track.state.direct_visibility_sample = false;
        refresh(track);
        ++it;
    }
    return true;
}

bool TemporalVisibilityModel::observe(const VisibilityObservation& observation) {
    if (!observation.id || !observation.frame || observation.frame < frame_ ||
        !probability(observation.confidence)) {
        return false;
    }
    if (observation.local_coverage_upper && !probability(*observation.local_coverage_upper)) return false;
    if (observation.visible_coverage && !probability(*observation.visible_coverage)) return false;
    if (observation.visible_coverage && observation.local_coverage_upper &&
        *observation.visible_coverage > *observation.local_coverage_upper + 1e-9) {
        return false;
    }
    if (observation.frame > frame_ && !advance(observation.frame)) return false;

    auto it = tracks_.find(observation.id);
    if (it == tracks_.end()) {
        if (tracks_.size() >= config_.max_tracks) return false;
        Track track{};
        track.state.id = observation.id;
        track.state.frame = observation.frame;
        track.state.frames_since_visible = (std::numeric_limits<std::uint32_t>::max)();
        track.last_state_frame = observation.frame;
        it = tracks_.emplace(observation.id, track).first;
    }

    auto& track = it->second;
    auto& state = track.state;
    const auto previous_frame = track.last_observed_frame;
    const double previous_coverage = track.smoothed_coverage;

    double raw_coverage = previous_coverage;
    double evidence_confidence = observation.confidence;
    bool direct = false;
    bool reachability_known = observation.present_reachable.has_value();

    if (observation.present_reachable && !*observation.present_reachable) {
        raw_coverage = 0.0;
        evidence_confidence *= 0.95;
        direct = true;
    } else if (observation.visible_coverage) {
        raw_coverage = *observation.visible_coverage;
        evidence_confidence *= 1.0;
        direct = true;
    } else if (observation.local_coverage_upper) {
        // A Stage 17 raster bound is only potential contribution. Use it as a
        // weak visibility proxy and expose correspondingly lower confidence.
        raw_coverage = *observation.local_coverage_upper;
        evidence_confidence *= 0.40;
    } else {
        evidence_confidence *= 0.20;
    }

    raw_coverage = clamp01(raw_coverage);
    const double alpha = previous_frame ? config_.coverage_alpha : 1.0;
    track.smoothed_coverage = clamp01(alpha * raw_coverage + (1.0 - alpha) * previous_coverage);

    if (previous_frame && observation.frame > previous_frame) {
        const double dt = static_cast<double>(observation.frame - previous_frame);
        const double instantaneous = (track.smoothed_coverage - previous_coverage) / dt;
        track.smoothed_velocity =
            config_.velocity_alpha * instantaneous +
            (1.0 - config_.velocity_alpha) * track.smoothed_velocity;
    } else if (!previous_frame) {
        track.smoothed_velocity = 0.0;
    }

    track.last_observed_frame = observation.frame;
    track.last_state_frame = observation.frame;
    state.frame = observation.frame;
    state.frames_since_observed = 0;
    state.direct_visibility_sample = direct;
    state.present_reachable_known = reachability_known;

    if (observation.local_coverage_upper) {
        state.coverage_upper = *observation.local_coverage_upper;
    } else {
        state.coverage_upper = std::max(state.coverage_upper, track.smoothed_coverage);
    }

    if (track.smoothed_coverage >= config_.visible_coverage && direct) {
        track.last_visible_frame = observation.frame;
    }
    state.frames_since_visible = track.last_visible_frame
        ? saturating_frames(observation.frame - track.last_visible_frame)
        : (std::numeric_limits<std::uint32_t>::max)();

    state.confidence = previous_frame
        ? clamp01(0.55 * state.confidence + 0.45 * evidence_confidence)
        : clamp01(evidence_confidence);

    refresh(track);
    return true;
}

const TemporalVisibilityState* TemporalVisibilityModel::find(VisualTrackId id) const noexcept {
    const auto it = tracks_.find(id);
    return it == tracks_.end() ? nullptr : &it->second.state;
}

std::vector<TemporalVisibilityState> TemporalVisibilityModel::snapshots() const {
    std::vector<TemporalVisibilityState> result;
    result.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_) {
        (void)id;
        result.push_back(track.state);
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return result;
}

void TemporalVisibilityModel::clear() noexcept {
    tracks_.clear();
    frame_ = 0;
}

}  // namespace arc
