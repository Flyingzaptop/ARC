#include "arc/memory_arbiter.hpp"

#include <iostream>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    // Prefer low visual-loss mip demotions over a more expensive whole-resource eviction.
    {
        MemoryArbiter arbiter;
        const std::vector<MemoryActionCandidate> candidates{
            {.kind=MemoryActionKind::EvictResource,.resource=1,.subject=101,.bytes_freed=100,.latency_risk_ms=.8,.confidence=.9},
            {.kind=MemoryActionKind::DemoteTexture,.resource=2,.subject=201,.sequence=0,.bytes_freed=60,.quality_loss=.01,.confidence=.95},
            {.kind=MemoryActionKind::DemoteTexture,.resource=2,.subject=201,.sequence=1,.bytes_freed=40,.quality_loss=.04,.confidence=.95},
        };
        const auto plan = arbiter.plan(80, candidates);
        CHECK(!plan.shortfall);
        CHECK(plan.actions.size() == 2);
        CHECK(plan.actions[0].candidate.kind == MemoryActionKind::DemoteTexture);
        CHECK(plan.actions[0].candidate.sequence == 0);
        CHECK(plan.actions[1].candidate.kind == MemoryActionKind::DemoteTexture);
        CHECK(plan.actions[1].candidate.sequence == 1);
        CHECK(plan.planned_bytes == 100);
    }

    // Sequential demotion dependency is strict even if a later step looks cheaper by itself.
    {
        MemoryArbiter arbiter;
        const std::vector<MemoryActionCandidate> candidates{
            {.kind=MemoryActionKind::DemoteTexture,.resource=5,.subject=50,.sequence=1,.bytes_freed=100,.quality_loss=0.0001},
            {.kind=MemoryActionKind::DemoteTexture,.resource=5,.subject=50,.sequence=0,.bytes_freed=10,.quality_loss=.01},
        };
        const auto plan = arbiter.plan(100, candidates);
        CHECK(plan.actions.size() == 2);
        CHECK(plan.actions[0].candidate.sequence == 0);
        CHECK(plan.actions[1].candidate.sequence == 1);
    }

    // A whole-resource eviction conflicts with every mip demotion for the same resource.
    {
        MemoryArbiterConfig config{};
        config.latency_weight = .01;
        MemoryArbiter arbiter(config);
        const std::vector<MemoryActionCandidate> candidates{
            {.kind=MemoryActionKind::EvictResource,.resource=9,.subject=90,.bytes_freed=200,.latency_risk_ms=.1,.confidence=1.0},
            {.kind=MemoryActionKind::DemoteTexture,.resource=9,.subject=91,.sequence=0,.bytes_freed=100,.quality_loss=.5,.confidence=1.0},
            {.kind=MemoryActionKind::DemoteTexture,.resource=10,.subject=100,.sequence=0,.bytes_freed=50,.quality_loss=.01,.confidence=1.0},
        };
        const auto plan = arbiter.plan(180, candidates);
        CHECK(!plan.shortfall);
        bool evicted9 = false, demoted9 = false;
        for (const auto& action : plan.actions) {
            if (action.candidate.resource == 9 && action.candidate.kind == MemoryActionKind::EvictResource) evicted9 = true;
            if (action.candidate.resource == 9 && action.candidate.kind == MemoryActionKind::DemoteTexture) demoted9 = true;
        }
        CHECK(evicted9 != demoted9);
    }

    // Ineligible/invalid candidates never satisfy the target and shortfall is reported honestly.
    {
        MemoryArbiter arbiter;
        const std::vector<MemoryActionCandidate> candidates{
            {.kind=MemoryActionKind::EvictResource,.resource=1,.subject=1,.bytes_freed=100,.eligible=false},
            {.kind=MemoryActionKind::EvictResource,.resource=2,.subject=2,.bytes_freed=0},
            {.kind=MemoryActionKind::DemoteTexture,.resource=3,.subject=3,.bytes_freed=40,.quality_loss=.01},
        };
        const auto plan = arbiter.plan(100, candidates);
        CHECK(plan.shortfall);
        CHECK(plan.planned_bytes == 40);
        CHECK(plan.actions.size() == 1);
    }

    // Max-action cap is a hard safety bound.
    {
        MemoryArbiterConfig config{};
        config.max_actions = 1;
        MemoryArbiter arbiter(config);
        const std::vector<MemoryActionCandidate> candidates{
            {.kind=MemoryActionKind::EvictResource,.resource=1,.subject=1,.bytes_freed=40,.latency_risk_ms=.1},
            {.kind=MemoryActionKind::EvictResource,.resource=2,.subject=2,.bytes_freed=40,.latency_risk_ms=.1},
        };
        const auto plan = arbiter.plan(80, candidates);
        CHECK(plan.shortfall);
        CHECK(plan.actions.size() == 1);
    }

    return 0;
}
