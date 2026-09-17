#include "arc/quality_profile.hpp"

#include <cassert>
#include <iostream>

using namespace arc;

int main() {
    QualityResourceProfile distant{};
    distant.id = 42;
    distant.domain = QualityDomain::Texture;
    distant.label = "distant wall";
    distant.semantic = QualitySemanticClass::Environment;
    distant.importance.screen_coverage = 0.03;
    distant.importance.visibility = 0.5;
    distant.importance.semantic_importance = 0.2;
    distant.importance.normalized_distance = 0.9;
    distant.levels = {
        {0.50, 0.40, 64ull << 20},
        {0.25, 0.25, 16ull << 20},
    };

    auto d = QualityCandidateFactory::build(distant);
    assert(d.size() == 2);
    assert(d[0].sequence == 0 && d[1].sequence == 1);
    assert(d[0].memory_freed_bytes == (64ull << 20));
    assert(!d[0].temporal_assist);

    QualityResourceProfile ui = distant;
    ui.id = 43;
    ui.label = "HUD";
    ui.semantic = QualitySemanticClass::Ui;
    ui.importance.screen_coverage = 0.05;
    ui.importance.visibility = 1.0;
    ui.importance.semantic_importance = 1.0;
    ui.importance.normalized_distance = 0.0;
    auto u = QualityCandidateFactory::build(ui);
    assert(!u.empty());
    assert(u[0].visual_cost > d[0].visual_cost);

    QualityResourceProfile temporal = distant;
    temporal.id = 44;
    temporal.domain = QualityDomain::Temporal;
    temporal.temporal_assist = true;
    auto t = QualityCandidateFactory::build(temporal);
    assert(!t.empty() && t[0].temporal_assist);

    std::cout << "quality-profile-tests: PASS\n";
    return 0;
}
