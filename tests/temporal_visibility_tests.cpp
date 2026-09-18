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
    const auto half = visible_coverage_from_occlusion(50, 100, 1);
    check(half && std::abs(*half - .5) < 1e-12, "occlusion coverage conversion");
    const auto msaa = visible_coverage_from_occlusion(200, 100, 4);
    check(msaa && std::abs(*msaa - .5) < 1e-12, "MSAA occlusion normalization");
    check(!visible_coverage_from_occlusion(1, 0, 1), "zero target rejected");
    check(!visible_coverage_from_occlusion(1, 100, 0), "zero sample count rejected");
    WorkObservation fingerprint_work{};
    fingerprint_work.kind = GpuWorkKind::Draw;
    fingerprint_work.pipeline = 77;
    fingerprint_work.raster = {1920, 1080, {0, 0, 1920, 1080}, {0, 0, 960, 1080}, true};
    fingerprint_work.accesses = {
        {11, false, AccessEvidence::Possible, false},
        {42, true, AccessEvidence::Observed, true},
    };
    fingerprint_work.bindings_complete = false;
    const auto fp_a = make_visual_track_fingerprint(fingerprint_work);
    const auto fp_b = make_visual_track_fingerprint(fingerprint_work);
    check(fp_a.id != 0 && fp_a.id == fp_b.id, "stable Stage D work fingerprint");
    check(fp_a.confidence > .5, "fingerprint confidence");

    AttributionNode node{};
    node.id = 1;
    node.work = fingerprint_work;
    node.local_coverage_upper = .5;
    node.unresolved_inputs = true;
    const auto weak_from_d = make_potential_visibility_observation(node, 1, true);
    check(weak_from_d.id == fp_a.id, "Stage D bridge track identity");
    check(weak_from_d.local_coverage_upper && *weak_from_d.local_coverage_upper == .5, "Stage D coverage forwarded");
    check(!weak_from_d.visible_coverage, "Stage D raster bound not promoted to actual visibility");
    check(weak_from_d.confidence < fp_a.confidence, "unresolved Stage D evidence lowers confidence");

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

    TemporalVisibilityModel steady_model;
    for (std::uint64_t frame = 1; frame <= 20; ++frame) {
        check(steady_model.observe(direct(77, frame, .10, .10)), "steady observation");
    }
    const auto* steady = steady_model.find(77);
    check(steady && std::abs(steady->estimated_visible_coverage - .10) < .005,
          "continuously observed coverage must not decay");

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
