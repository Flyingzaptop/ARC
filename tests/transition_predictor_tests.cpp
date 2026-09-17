#include "arc/transition_predictor.hpp"

#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    ResourceTransitionPredictor predictor;
    for (int repeat = 0; repeat < 12; ++repeat) {
        predictor.observe(1);
        predictor.observe(2);
        predictor.observe(3);
    }
    predictor.reset_context();
    predictor.observe(1);
    const auto after1 = predictor.predictions();
    CHECK(!after1.empty());
    CHECK(after1.front().resource == 2);
    CHECK(after1.front().order == 1);
    CHECK(after1.front().confidence >= predictor.config().minimum_confidence);

    // Order-2 context disambiguates an otherwise ambiguous B -> {C,E} transition.
    ResourceTransitionPredictor contextual;
    for (int repeat = 0; repeat < 16; ++repeat) {
        contextual.reset_context();
        contextual.observe(10);
        contextual.observe(20);
        contextual.observe(30);
        contextual.reset_context();
        contextual.observe(40);
        contextual.observe(20);
        contextual.observe(50);
    }
    contextual.reset_context();
    contextual.observe(10);
    contextual.observe(20);
    auto predictions = contextual.predictions();
    CHECK(!predictions.empty());
    CHECK(predictions.front().order == 2);
    CHECK(predictions.front().resource == 30);
    contextual.reset_context();
    contextual.observe(40);
    contextual.observe(20);
    predictions = contextual.predictions();
    CHECK(!predictions.empty());
    CHECK(predictions.front().order == 2);
    CHECK(predictions.front().resource == 50);

    // Bounded counters adapt to a phase change instead of preserving old history forever.
    TransitionPredictorConfig adaptive_config{};
    adaptive_config.minimum_context_observations = 2;
    adaptive_config.max_context_samples = 16;
    adaptive_config.minimum_confidence = 0.10;
    ResourceTransitionPredictor adaptive(adaptive_config);
    for (int repeat = 0; repeat < 24; ++repeat) {
        adaptive.reset_context();
        adaptive.observe(100);
        adaptive.observe(200);
    }
    for (int repeat = 0; repeat < 80; ++repeat) {
        adaptive.reset_context();
        adaptive.observe(100);
        adaptive.observe(300);
    }
    adaptive.reset_context();
    adaptive.observe(100);
    predictions = adaptive.predictions();
    CHECK(!predictions.empty());
    CHECK(predictions.front().resource == 300);
    CHECK(predictions.front().probability > 0.70);

    // Resource id 0 is a context boundary and must not become a prediction target.
    adaptive.observe(0);
    CHECK(adaptive.predictions().empty());

    return 0;
}
