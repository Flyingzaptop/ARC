#include "arc/adaptive_quality_controller.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

using namespace arc;

namespace {

struct Phase {
    double full_quality_ms{};
    int samples{};
};

struct ActiveKey {
    std::uint64_t id{};
    std::uint32_t sequence{};
    friend bool operator==(const ActiveKey&, const ActiveKey&) = default;
};

struct ActiveHash {
    std::size_t operator()(const ActiveKey& k) const noexcept {
        return static_cast<std::size_t>(k.id ^ (static_cast<std::uint64_t>(k.sequence) << 32));
    }
};

double noise_for(int sample) {
    // Deterministic +/- ~0.12 ms jitter; no RNG means the CI result is stable.
    return 0.12 * std::sin(static_cast<double>(sample) * 0.73) +
           0.04 * std::cos(static_cast<double>(sample) * 0.19);
}

FrameBudgetSample make_sample(double frame_ms, double target_ms) {
    FrameBudgetSample s{};
    s.frame_ms = frame_ms;
    s.target_frame_ms = target_ms;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.40;
    s.raster_pressure = 0.40;
    s.geometry_pressure = 0.40;
    s.lighting_pressure = 0.40;
    return s;
}

} // namespace

int main() {
    constexpr double target_ms = 16.7;
    const std::vector<Phase> phases{
        {12.0, 30}, // easy scene, full quality
        {21.5, 55}, // enter a heavy room
        {13.0, 55}, // leave it: quality should recover
        {23.0, 65}, // harder scene
        {12.5, 65}, // easy again: recover to full quality
    };

    std::vector<QualityActionCandidate> candidates{
        {100, QualityDomain::Raster,    "distant overdraw",       2.8, 0.050, 0.96, 0, true, false, 0},
        {200, QualityDomain::Lighting,  "far light iterations",   2.1, 0.045, 0.95, 0, true, false, 0},
        {300, QualityDomain::Geometry,  "distant mesh density",   1.6, 0.040, 0.94, 0, true, false, 0},
        {400, QualityDomain::Bandwidth, "far texture sampling",   1.1, 0.038, 0.94, 0, true, false, 0},
        {500, QualityDomain::Shadow,    "far shadow frequency",   0.9, 0.042, 0.93, 0, true, false, 0},
        // Must remain unavailable unless the user explicitly opts in.
        {900, QualityDomain::Temporal,  "temporal assist",        9.0, 0.001, 1.00, 0, true, true, 0},
    };

    AdaptiveQualityControllerConfig cfg{};
    cfg.optimizer.allow_temporal_assist = false;
    cfg.optimizer.minimum_gain_ms = 0.05;
    cfg.optimizer.minimum_confidence = 0.50;
    cfg.optimizer.restoration_headroom_ms = 1.0;
    cfg.overload_samples_required = 3;
    cfg.headroom_samples_required = 5;
    cfg.settle_samples_after_change = 2;
    cfg.minimum_hold_samples_after_degrade = 7;
    cfg.frame_ewma_alpha = 0.45;
    cfg.overload_margin_ms = 0.15;
    cfg.extra_restore_headroom_ms = 0.25;
    cfg.max_actions_per_decision = 1;
    AdaptiveQualityController controller{cfg};

    std::unordered_map<ActiveKey, QualityActionCandidate, ActiveHash> applied;
    double active_gain_ms = 0.0;
    std::uint64_t baseline_overloaded = 0;
    std::uint64_t arc_overloaded = 0;
    std::uint64_t total = 0;
    std::uint64_t degrade_events = 0;
    std::uint64_t restore_events = 0;
    bool temporal_seen = false;
    int global_sample = 0;

    for (const auto& phase : phases) {
        for (int i = 0; i < phase.samples; ++i, ++global_sample) {
            const double jitter = noise_for(global_sample);
            const double baseline_ms = phase.full_quality_ms + jitter;
            const double frame_ms = std::max(0.1, baseline_ms - active_gain_ms);
            baseline_overloaded += baseline_ms > target_ms ? 1u : 0u;
            arc_overloaded += frame_ms > target_ms ? 1u : 0u;
            ++total;

            const auto decision = controller.tick(make_sample(frame_ms, target_ms), candidates);
            if (decision.kind == QualityDecisionKind::None) continue;
            assert(decision.plan.actions.size() == 1);
            const auto action = decision.plan.actions.front();
            temporal_seen = temporal_seen || action.temporal_assist || action.domain == QualityDomain::Temporal;
            assert(!temporal_seen);

            const ActiveKey key{action.id, action.sequence};
            if (decision.kind == QualityDecisionKind::Degrade) {
                assert(!applied.contains(key));
                const double before = frame_ms;
                active_gain_ms += action.expected_ms_gain;
                const double after = std::max(0.1, before - action.expected_ms_gain);
                applied.emplace(key, action);
                controller.note_action_applied(action, decision.kind, before, after, true);
                ++degrade_events;
            } else {
                assert(applied.contains(key));
                const double before = frame_ms;
                active_gain_ms = std::max(0.0, active_gain_ms - action.expected_ms_gain);
                const double after = before + action.expected_ms_gain;
                applied.erase(key);
                controller.note_action_applied(action, decision.kind, before, after, true);
                ++restore_events;
            }
        }
    }

    // Closed loop must materially reduce time spent over the frame budget.
    assert(baseline_overloaded > 0);
    assert(arc_overloaded * 2 < baseline_overloaded);
    assert(degrade_events >= 4);
    assert(restore_events >= 4);
    assert(!temporal_seen);

    // The final easy phase is long enough to recover all sacrificed quality.
    assert(controller.active_actions().empty());
    assert(applied.empty());
    assert(active_gain_ms < 1e-9);

    const auto state = controller.state();
    assert(state.degrade_actions_applied == degrade_events);
    assert(state.restore_actions_applied == restore_events);
    // Two heavy/easy transitions should not turn into frame-by-frame chatter.
    assert(state.direction_changes <= 4);

    std::cout << "closed-loop-quality-sim-tests: PASS baseline_overloaded="
              << baseline_overloaded << " arc_overloaded=" << arc_overloaded
              << " degrade=" << degrade_events << " restore=" << restore_events
              << " direction_changes=" << state.direction_changes << "\n";
    return 0;
}
