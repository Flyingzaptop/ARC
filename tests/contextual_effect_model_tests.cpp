#include "arc/contextual_effect_model.hpp"

#include <cassert>
#include <iostream>

using namespace arc;

int main() {
    ContextualActionEffectTracker model{};
    QualityActionCandidate action{
        101,
        QualityDomain::Geometry,
        "contextual geometry",
        1.0,
        0.05,
        0.9,
        0,
        true,
        false,
        0
    };

    FrameBudgetSample geometry{};
    geometry.frame_ms = 20.0;
    geometry.target_frame_ms = 16.7;
    geometry.gpu_busy_fraction = 0.98;
    geometry.geometry_pressure = 1.0;
    geometry.raster_pressure = 0.30;
    geometry.memory_bandwidth_fraction = 0.10;

    FrameBudgetSample bandwidth = geometry;
    bandwidth.geometry_pressure = 0.05;
    bandwidth.raster_pressure = 0.10;
    bandwidth.memory_bandwidth_fraction = 1.0;

    for (int i = 0; i < 120; ++i) {
        model.record(action, geometry, 3.0);
        model.record(action, bandwidth, 0.35);
    }

    const auto g = model.predict(action, geometry);
    const auto b = model.predict(action, bandwidth);
    assert(g.has_value());
    assert(b.has_value());
    assert(g->samples == 240);
    assert(g->predicted_ms > b->predicted_ms + 1.0);
    assert(g->predicted_ms > 1.5);
    assert(b->predicted_ms < 1.5);

    const auto calibrated_g = model.calibrate(action, geometry);
    const auto calibrated_b = model.calibrate(action, bandwidth);
    assert(calibrated_g.expected_ms_gain > calibrated_b.expected_ms_gain);
    assert(calibrated_g.confidence >= 0.0 && calibrated_g.confidence <= 1.0);

    model.clear();
    assert(!model.predict(action, geometry).has_value());

    std::cout << "contextual-effect-model-tests: PASS\n";
    return 0;
}
