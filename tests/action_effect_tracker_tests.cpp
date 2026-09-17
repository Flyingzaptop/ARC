#include "arc/action_effect_tracker.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace arc;

int main() {
    QualityActionCandidate action{};
    action.id = 42;
    action.sequence = 0;
    action.expected_ms_gain = 1.0;
    action.confidence = 0.8;

    ActionEffectTracker tracker{2.0};
    auto untouched = tracker.calibrate(action);
    assert(std::abs(untouched.expected_ms_gain - 1.0) < 1e-9);

    tracker.record(action, 2.0);
    tracker.record(action, 2.2);
    tracker.record(action, 1.8);
    tracker.record(action, 2.1);

    auto stats = tracker.find(action);
    assert(stats.has_value());
    assert(stats->samples == 4);
    assert(stats->mean_gain_ms > 1.9 && stats->mean_gain_ms < 2.1);
    assert(stats->confidence > 0.0);

    auto calibrated = tracker.calibrate(action);
    assert(calibrated.expected_ms_gain > action.expected_ms_gain);
    assert(calibrated.expected_ms_gain < 2.2);

    QualityActionCandidate other = action;
    other.sequence = 1;
    assert(!tracker.find(other).has_value());

    std::cout << "action-effect-tracker-tests: PASS\n";
    return 0;
}
