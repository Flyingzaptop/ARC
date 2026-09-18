#include "arc/temporal_visibility.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace arc;

namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

VisibilityObservation direct(
    VisualTrackId id,
    std::uint64_t frame,
    double upper,
    double visible,
    double confidence = 1.0) {
    VisibilityObservation o{};
    o.id = id;
    o.frame = frame;
    o.local_coverage_upper = upper;
    o.visible_coverage = visible;
    o.present_reachable = true;
    o.confidence = confidence;
    return o;
}
}

int main() {
    TemporalVisibilityModel model;

    check(model.observe(direct(1, 1, .01, .01)), "first observation");
    check(model.observe(direct(1, 2, .03, .03)), "second observation");
    check(model.observe(direct(1, 3, .07, .07)), "third observation");

    const auto* entering = model.find(1);
    check(entering != nullptr, "entering state exists");
    check(entering->phase == VisibilityPhase::Entering, "growing object should enter");
    check(entering->coverage_velocity > 0.0, "entering velocity");
    check(entering->predicted_8f > entering->estimated_visible_coverage, "future growth predicted");
    check(entering->temporal_relevance >= entering->estimated_visible_coverage, "future relevance retained");
    check(entering->confidence > .7, "direct evidence confidence");

    check(model.observe(direct(2, 3, .50, 0.0)), "occluded observation");
    const auto* occluded = model.find(2);
    check(occluded && occluded->phase == VisibilityPhase::Hidden, "direct occlusion wins over potential raster area");
    check(occluded->estimated_visible_coverage <= 1e-9, "occluded visible coverage zero");
    check(occluded->coverage_upper >= .49, "potential coverage retained separately");

    check(model.observe(direct(3, 3, .10, .10)), "leaving initial");
    check(model.observe(direct(3, 4, .05, .05)), "leaving second");
    check(model.observe(direct(3, 5, .005, .005)), "leaving third");
    const auto* leaving = model.find(3);
    check(leaving && leaving->phase == VisibilityPhase::Leaving, "shrinking object should leave");
    check(leaving->coverage_velocity < 0.0, "leaving velocity");
    check(leaving->predicted_8f < leaving->estimated_visible_coverage, "leaving prediction decreases");

    VisibilityObservation weak{};
    weak.id = 4;
    weak.frame = 5;
    weak.local_coverage_upper = .25;
    weak.confidence = .8;
    check(model.observe(weak), "weak raster-only observation");
    const auto* potential = model.find(4);
    check(potential && !potential->direct_visibility_sample, "raster bound is not direct visibility");
    check(potential->confidence < .5, "raster-only evidence confidence reduced");

    VisibilityObservation unreachable{};
    unreachable.id = 5;
    unreachable.frame = 5;
    unreachable.local_coverage_upper = .4;
    unreachable.present_reachable = false;
    unreachable.confidence = 1.0;
    check(model.observe(unreachable), "unreachable observation");
    const auto* hidden = model.find(5);
    check(hidden && hidden->phase == VisibilityPhase::Hidden, "known unreachable hidden");
    check(hidden->present_reachable_known, "reachability provenance");

    const double before_decay = entering->estimated_visible_coverage;
    check(model.advance(12), "advance missing frames");
    entering = model.find(1);
    check(entering && entering->estimated_visible_coverage < before_decay, "missing observation decays");
    check(entering->frames_since_observed == 9, "missing frame count");
    check(entering->confidence < 1.0, "missing confidence decay");

    VisibilityObservation invalid = direct(9, 12, .1, .2);
    check(!model.observe(invalid), "visible cannot exceed upper bound");
    invalid = direct(9, 11, .2, .1);
    check(!model.observe(invalid), "old frame rejected");

    TemporalVisibilityConfig tiny{};
    tiny.max_tracks = 1;
    TemporalVisibilityModel bounded(tiny);
    check(bounded.observe(direct(1, 1, .1, .1)), "bounded first");
    check(!bounded.observe(direct(2, 1, .1, .1)), "bounded capacity fail closed");

    TemporalVisibilityConfig stale{};
    stale.stale_frames = 3;
    TemporalVisibilityModel pruning(stale);
    check(pruning.observe(direct(10, 1, .1, .1)), "stale first");
    check(pruning.advance(5), "stale advance");
    check(pruning.find(10) == nullptr, "stale track pruned");

    for (const auto& snapshot : model.snapshots()) {
        check(std::isfinite(snapshot.estimated_visible_coverage), "finite coverage");
        check(std::isfinite(snapshot.coverage_velocity), "finite velocity");
        check(snapshot.predicted_2f >= 0.0 && snapshot.predicted_2f <= 1.0, "bounded p2");
        check(snapshot.predicted_8f >= 0.0 && snapshot.predicted_8f <= 1.0, "bounded p8");
        check(snapshot.predicted_30f >= 0.0 && snapshot.predicted_30f <= 1.0, "bounded p30");
        check(snapshot.temporal_relevance >= 0.0 && snapshot.temporal_relevance <= 1.0, "bounded relevance");
        check(snapshot.confidence >= 0.0 && snapshot.confidence <= 1.0, "bounded confidence");
    }

    std::cout << "Stage 18 temporal visibility tests PASS\n";
    return 0;
}
