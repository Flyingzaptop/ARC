#include "arc/discovery_budget.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    using Budget = arc::DiscoveryBudget;
    const auto base = Budget::Time{} + 10s;
    Budget::Config config;
    config.outstanding_jobs = 2;
    config.outstanding_bytes = 128;
    config.capture_events = 3;
    config.capture_bytes = 8;
    Budget budget(config);

    auto first = budget.try_reserve(80);
    assert(first && first->valid());
    assert(!budget.try_reserve(80));  // bytes are bounded, including queued work
    auto second = budget.try_reserve(40);
    assert(second && !budget.try_reserve(1));
    assert(budget.snapshot().jobs_high_water == 2);
    assert(budget.snapshot().bytes_high_water == 120);

    auto capture = budget.try_begin_capture(base);
    assert(capture && !budget.try_begin_capture(base));
    assert(capture->record(1, 2, base + 1ms));
    assert(!capture->record(2, 2, base + 2ms));  // count boundary invalidates trace
    assert(capture->incomplete() && !capture->finish(base + 2ms));
    assert(budget.snapshot().incomplete_captures == 1);
    assert(!budget.try_begin_capture(base + 999ms));
    auto next = budget.try_begin_capture(base + 1s);
    assert(next && next->record(1, 2, base + 1001ms));
    assert(next->finish(base + 1002ms));  // bounded complete region
    assert(!next->incomplete());  // finish preserves the result after release

    budget.discovery_finished(false, base + 1s);
    budget.note_novelty();
    assert(!budget.ready(base + 4s));
    assert(budget.ready(base + 5s));
    budget.discovery_finished(false, base + 5s);
    assert(!budget.ready(base + 12s) && budget.ready(base + 13s));

    budget.cancel();
    assert(!first->valid() && !second->valid());
    assert(!budget.ready(base + 50s));
    auto fresh = budget.try_reserve(4);
    assert(!fresh);  // old leases still hold bounded queue capacity
    first.reset(); second.reset();
    fresh = budget.try_reserve(4);
    assert(fresh && fresh->valid() && fresh->epoch() == budget.snapshot().epoch);
    fresh.reset();
    assert(budget.snapshot().active_jobs == 0 && budget.snapshot().bytes_in_flight == 0);
    assert(budget.snapshot().rejected_jobs == 3);
    std::cout << "Discovery quotas, capture completeness, cooldown and cancellation PASS\n";
}
