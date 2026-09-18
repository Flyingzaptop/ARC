#include "arc/temporal_visibility.hpp"
#include "arc/visual_importance.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace arc;

namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

VisibilityObservation sample(
    VisualTrackId id,
    std::uint64_t frame,
    double upper,
    double visible,
    double confidence = .95) {
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
    TemporalVisibilityModel temporal;
    VisualImportanceModel importance;

    // Synthetic camera turn. Ground-truth trajectories are used only by this
    // test; no object label/name enters either inference model.
    const std::vector<double> entering{0.0, .001, .004, .012, .030, .060, .095, .130};
    const std::vector<double> leaving{.130, .100, .070, .040, .020, .008, .002, 0.0};
    const std::vector<double> occluded{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    double first_enter_importance{};
    double last_enter_importance{};
    double first_leave_importance{};
    double last_leave_importance{};

    for (std::size_t i = 0; i < entering.size(); ++i) {
        const auto frame = static_cast<std::uint64_t>(i + 1);
        check(temporal.observe(sample(101, frame, std::max(.14, entering[i]), entering[i])), "entering sample");
        check(temporal.observe(sample(202, frame, std::max(.14, leaving[i]), leaving[i])), "leaving sample");
        check(temporal.observe(sample(303, frame, .25, occluded[i])), "occluded sample");

        const auto* a = temporal.find(101);
        const auto* b = temporal.find(202);
        const auto* c = temporal.find(303);
        check(a && b && c, "all tracks");

        VisualImportanceHint center{};
        center.screen_x = 0.0;
        center.screen_y = 0.0;
        center.composition_relevance = 1.0;
        center.perceptual_sensitivity = 1.0;

        const auto ia = importance.evaluate(*a, center);
        const auto ib = importance.evaluate(*b, center);
        const auto ic = importance.evaluate(*c, center);

        if (i == 0) {
            first_enter_importance = ia.score;
            first_leave_importance = ib.score;
        }
        if (i + 1 == entering.size()) {
            last_enter_importance = ia.score;
            last_leave_importance = ib.score;
        }

        check(ic.score < .03, "occluded object remains low");
    }

    const auto* enter_state = temporal.find(101);
    const auto* leave_state = temporal.find(202);
    check(enter_state && leave_state, "final states");
    check(enter_state->phase == VisibilityPhase::Entering || enter_state->phase == VisibilityPhase::Visible,
          "approaching/turning object enters");
    check(leave_state->phase == VisibilityPhase::Leaving || leave_state->phase == VisibilityPhase::Hidden,
          "departing object leaves");
    check(last_enter_importance > first_enter_importance, "importance rises before/while entering");
    check(last_leave_importance < first_leave_importance, "importance falls while leaving");

    // Recently visible work should not collapse to zero on a single missing
    // sample: this is the precondition for future prefetch/restore logic.
    const double before_gap = enter_state->temporal_relevance;
    check(temporal.advance(9), "single-frame gap");
    enter_state = temporal.find(101);
    check(enter_state && enter_state->temporal_relevance > 0.0, "recent visibility survives brief gap");
    check(enter_state->temporal_relevance <= before_gap + .25, "gap remains bounded");

    // But sustained absence must decay.
    check(temporal.advance(40), "long gap");
    enter_state = temporal.find(101);
    check(enter_state && enter_state->estimated_visible_coverage < .01, "long absence decays visible estimate");

    std::cout << "Mega E Stage 18+19 scenario tests PASS\n";
    return 0;
}
