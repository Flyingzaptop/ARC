#include "arc/discovery_budget.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    {
        arc::DiscoveryBudget::Config exact_config;
        exact_config.initial_probe=false;
        arc::DiscoveryBudget exact(exact_config);
        assert(exact.note_novelty(123));
        assert(!exact.note_novelty((1ull<<62)+123));
        assert(!exact.try_begin_capture(arc::DiscoveryBudget::Clock::now(),(1ull<<62)+123));
        assert(exact.snapshot().pending_novelty==1);
    }
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

    // Two known regions survive completion of the first capture. Repeated
    // reports of one region must not create duplicate tickets.
    Budget queued(config);
    assert(queued.note_novelty(101));
    assert(queued.note_novelty(202));
    assert(!queued.note_novelty(202));
    assert(queued.snapshot().pending_novelty == 3); // initial + two keys
    auto q0 = queued.try_begin_capture(base);
    assert(q0 && q0->finish(base + 1ms));
    queued.discovery_finished(true, base + 1ms, q0->epoch());
    assert(queued.snapshot().pending_novelty == 2);
    auto q1 = queued.try_begin_capture(base + 2s);
    assert(q1 && q1->finish(base + 2001ms));
    queued.discovery_finished(true, base + 2001ms, q1->epoch());
    assert(queued.snapshot().pending_novelty == 1);
    auto q2 = queued.try_begin_capture(base + 60s);
    assert(q2 && q2->finish(base + 60001ms));
    queued.discovery_finished(true, base + 60001ms, q2->epoch());
    assert(!queued.ready(base + 120s));

    // Novelty discovered while work runs remains after that work completes.
    Budget raced(config);
    auto active = raced.try_begin_capture(base);
    assert(active);
    assert(raced.note_novelty(303));
    assert(!raced.note_novelty(303));
    assert(active->finish(base + 1ms));
    raced.discovery_finished(true, base + 1ms, active->epoch());
    assert(raced.snapshot().pending_novelty == 1);
    assert(raced.try_begin_capture(base + 2s));

    // A completion from an invalidated epoch cannot consume new work.
    Budget epochs(config);
    auto stale = epochs.try_begin_capture(base);
    assert(stale);
    const auto old_epoch = stale->epoch();
    epochs.cancel();
    assert(!epochs.note_novelty(404));
    stale.reset();
    assert(epochs.resume());
    assert(epochs.note_novelty(404));
    epochs.discovery_finished(true, base + 1ms, old_epoch);
    assert(epochs.snapshot().pending_novelty == 1);
    assert(epochs.try_begin_capture(base + 2s));

    // Completed slots are reusable indefinitely, including the legacy token.
    Budget reusable(config);
    auto boot = reusable.try_begin_capture(base);
    assert(boot && boot->finish(base + 1ms));
    reusable.discovery_finished(true, base + 1ms, boot->epoch(), boot->ticket());
    for (std::uint64_t i = 1; i <= 300; ++i) {
        assert(reusable.note_novelty(1000 + i));
        assert(!reusable.note_novelty(1000 + i));
        const auto at = base + std::chrono::seconds(i * 2);
        auto work = reusable.try_begin_capture(at);
        assert(work && work->finish(at + 1ms));
        reusable.discovery_finished(true, at + 1ms, work->epoch(), work->ticket());
        assert(reusable.snapshot().pending_novelty == 0);
    }
    reusable.note_novelty();
    auto legacy = reusable.try_begin_capture(base + 602s);
    assert(legacy && legacy->finish(base + 602001ms));
    reusable.discovery_finished(true, base + 602001ms, legacy->epoch(), legacy->ticket());
    reusable.note_novelty();
    assert(reusable.snapshot().pending_novelty == 1);
    assert(reusable.try_begin_capture(base + 604s));
    assert(reusable.snapshot().novelty_overflow == 0);

    Budget duplicate(config);
    auto initial = duplicate.try_begin_capture(base);
    assert(initial && initial->finish(base + 1ms));
    duplicate.discovery_finished(true, base + 1ms, initial->epoch(), initial->ticket());
    std::thread reporters[8];
    for (auto& reporter : reporters) reporter = std::thread([&] {
        for (int i = 0; i < 100; ++i) duplicate.note_novelty(0xabcde);
    });
    for (auto& reporter : reporters) reporter.join();
    assert(duplicate.snapshot().pending_novelty == 1);

    // A freed collision slot must not hide a still-pending matching key.
    auto bucket = [](std::uint64_t key) {
        key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ull;
        key ^= key >> 27; key *= 0x94d049bb133111ebull;
        key ^= key >> 31;
        const auto id = key & ((1ull << 62) - 1);
        return (id ? id : 1) % 256;
    };
    std::uint64_t left = 1;
    while (bucket(left) == 255) ++left;
    std::uint64_t right = left + 1;
    while (bucket(right) != bucket(left)) ++right;
    Budget holes(config);
    auto seed = holes.try_begin_capture(base);
    assert(seed && seed->finish(base + 1ms));
    holes.discovery_finished(true, base + 1ms, seed->epoch(), seed->ticket());
    assert(holes.note_novelty(left) && holes.note_novelty(right));
    auto first_key = holes.try_begin_capture(base + 2s);
    assert(first_key && first_key->finish(base + 2001ms));
    holes.discovery_finished(true, base + 2001ms, first_key->epoch(), first_key->ticket());
    assert(holes.snapshot().pending_novelty == 1);

    // Keyed CPU capture must consume the requested region's ticket, with no
    // bootstrap phantom and no cross-region consumption.
    Budget::Config keyed_config = config;
    keyed_config.initial_probe = false;
    Budget keyed(keyed_config);
    assert(!keyed.ready(base));
    assert(keyed.note_novelty(101) && keyed.note_novelty(202));
    assert(!keyed.try_begin_capture(base, 303));
    auto region_a = keyed.try_begin_capture(base, 101);
    assert(region_a && region_a->finish(base + 1ms));
    keyed.discovery_finished(true, base + 1ms, region_a->epoch(), region_a->ticket());
    assert(keyed.snapshot().pending_novelty == 1);
    assert(!keyed.try_begin_capture(base + 2s, 101));
    auto region_b = keyed.try_begin_capture(base + 2s, 202);
    assert(region_b && region_b->finish(base + 2001ms));
    keyed.discovery_finished(true, base + 2001ms, region_b->epoch(), region_b->ticket());
    assert(keyed.snapshot().pending_novelty == 0);
    assert(!keyed.try_begin_capture(base + 4s, 101));
    assert(keyed.note_novelty(101)); // completion freed its ticket
    assert(keyed.try_begin_capture(base + 4s, 101));

    Budget evictions(keyed_config);
    for (std::uint64_t key = 1; key <= 300; ++key) {
        assert(evictions.note_novelty(key));
        assert(evictions.forget_novelty(key));
        assert(evictions.snapshot().pending_novelty == 0);
    }
    assert(evictions.snapshot().novelty_overflow == 0);
    assert(evictions.note_novelty(901) && evictions.note_novelty(902));
    auto owned = evictions.try_begin_capture(base, 901);
    assert(owned);
    assert(!evictions.forget_novelty(901)); // active ticket is protected
    assert(evictions.forget_novelty(902));
    assert(evictions.snapshot().pending_novelty == 0);
    assert(owned->finish(base + 1ms));
    evictions.discovery_finished(true, base + 1ms, owned->epoch(), owned->ticket());
    assert(evictions.snapshot().pending_novelty == 0);
    assert(!holes.note_novelty(right));
    assert(holes.snapshot().pending_novelty == 1);
    std::cout << "Discovery quotas, capture completeness, cooldown and cancellation PASS\n";
}
