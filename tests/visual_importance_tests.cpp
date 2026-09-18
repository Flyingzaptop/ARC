#include "arc/visual_importance.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace arc;

namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

TemporalVisibilityState state(
    VisualTrackId id,
    VisibilityPhase phase,
    double current,
    double p2,
    double p8,
    double p30,
    double relevance,
    double confidence) {
    TemporalVisibilityState s{};
    s.id = id;
    s.phase = phase;
    s.estimated_visible_coverage = current;
    s.predicted_2f = p2;
    s.predicted_8f = p8;
    s.predicted_30f = p30;
    s.temporal_relevance = relevance;
    s.confidence = confidence;
    return s;
}
}

int main() {
    VisualImportanceModel model;

    VisualImportanceHint center{};
    center.screen_x = 0.0;
    center.screen_y = 0.0;
    center.perceptual_sensitivity = 1.0;
    center.composition_relevance = 1.0;

    auto big = model.evaluate(state(1, VisibilityPhase::Visible, .36, .36, .36, .36, .36, .95), center);
    auto tiny = model.evaluate(state(2, VisibilityPhase::Visible, .0004, .0004, .0004, .0004, .0004, .95), center);
    check(big.score > tiny.score * 5.0, "coverage should materially affect importance");

    VisualImportanceHint edge = center;
    edge.screen_x = 1.0;
    edge.screen_y = 1.0;
    auto central = model.evaluate(state(3, VisibilityPhase::Visible, .05, .05, .05, .05, .05, .95), center);
    auto peripheral = model.evaluate(state(3, VisibilityPhase::Visible, .05, .05, .05, .05, .05, .95), edge);
    check(central.score > peripheral.score, "central contribution should be at least modestly more important");

    auto entering = model.evaluate(state(4, VisibilityPhase::Entering, .01, .03, .08, .12, .10, .90), center);
    auto leaving = model.evaluate(state(5, VisibilityPhase::Leaving, .01, .005, 0.0, 0.0, .01, .90), center);
    check(entering.score > leaving.score, "predicted entry should raise future importance");

    auto hidden = model.evaluate(state(6, VisibilityPhase::Hidden, 0.0, 0.0, 0.0, 0.0, 0.0, .95), center);
    check(hidden.score < .02, "known hidden work should be low importance");

    VisualImportanceHint low_sensitivity = center;
    low_sensitivity.perceptual_sensitivity = .25;
    auto sensitive = model.evaluate(state(7, VisibilityPhase::Visible, .10, .10, .10, .10, .10, .95), center);
    auto insensitive = model.evaluate(state(7, VisibilityPhase::Visible, .10, .10, .10, .10, .10, .95), low_sensitivity);
    check(sensitive.score > insensitive.score, "sensitivity hint should affect score");

    auto high_conf = model.evaluate(state(8, VisibilityPhase::Unknown, .02, .02, .02, .02, .02, .95), center);
    auto low_conf = model.evaluate(state(8, VisibilityPhase::Unknown, .02, .02, .02, .02, .02, .10), center);
    check(low_conf.uncertainty_component > high_conf.uncertainty_component, "uncertainty reserve should rise when confidence falls");
    check(low_conf.score >= high_conf.score, "uncertainty must not make unknown work look artificially cheaper");

    VisualImportanceHint unknown{};
    auto conservative = model.evaluate(state(9, VisibilityPhase::Visible, .04, .04, .04, .04, .04, .40), unknown);
    check(conservative.conservative_unknowns, "unknown hints must be marked conservative");
    check(conservative.sensitivity_component == 1.0, "unknown sensitivity defaults conservative");
    check(conservative.composition_component == 1.0, "unknown composition defaults conservative");
    check(conservative.centrality_component == 1.0, "unknown position defaults conservative");

    for (const auto& value : {big, tiny, central, peripheral, entering, leaving, hidden, sensitive, insensitive, high_conf, low_conf, conservative}) {
        check(std::isfinite(value.score), "importance finite");
        check(value.score >= 0.0 && value.score <= 1.0, "importance bounded");
        check(value.confidence >= 0.0 && value.confidence <= 1.0, "confidence bounded");
        check(value.coverage_component >= 0.0 && value.coverage_component <= 1.0, "coverage bounded");
        check(value.temporal_component >= 0.0 && value.temporal_component <= 1.0, "temporal bounded");
    }

    std::cout << "Stage 19 visual importance tests PASS\n";
    return 0;
}
