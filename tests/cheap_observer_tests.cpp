#include "arc/cheap_observer.hpp"
#include "arc/intercept_cpu_meter.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>

int main() {
    arc::CheapObserver observer;
    const auto original_epoch = observer.correctness_epoch();
    for (std::uint64_t i = 0; i < 65; ++i) observer.notify_creation(7, i, 1024 + i);
    auto snapshot = observer.snapshot();
    assert(snapshot.creations == 65 && snapshot.novelty == 65);
    assert(snapshot.diagnostic_drops == 1 && snapshot.claimed_slots_high_water == 1);
    assert(snapshot.correctness_epoch == original_epoch); // diagnostic overflow is separate
    std::uint64_t drained{};
    assert(observer.drain([&](const arc::Event& event) {
        assert(event.header.type == arc::EventType::TelemetrySample && event.header.flags == 1);
        assert(event.header.payload_bytes == sizeof(arc::CheapObserver::CreationHint));
        arc::CheapObserver::CreationHint hint;
        std::memcpy(&hint, event.payload.data(), sizeof(hint));
        assert(hint.kind == 7 && hint.opaque_identity == drained && hint.size == 1024 + drained);
        assert(event.header.timestamp_ns != 0 && event.header.sequence == drained + 1 && event.header.thread_id != 0);
        ++drained;
    }) == 64);
    assert(drained == 64 && observer.drain([](const arc::Event&) {}) == 0);
    observer.mark_correctness_loss();
    assert(observer.correctness_epoch() == original_epoch + 1);

    // Reusing an address must not reuse a thread-local producer slot from a
    // destroyed observer instance.
    alignas(arc::CheapObserver) std::byte storage[sizeof(arc::CheapObserver)];
    auto* first = new (storage) arc::CheapObserver;
    first->notify_creation(1, 11, 1);
    first->~CheapObserver();
    auto* second = new (storage) arc::CheapObserver;
    second->notify_creation(2, 22, 2);
    assert(second->snapshot().claimed_slots_high_water == 1);
    assert(second->drain([](const arc::Event& event) {
        arc::CheapObserver::CreationHint hint;
        std::memcpy(&hint, event.payload.data(), sizeof(hint));
        assert(hint.kind == 2 && hint.opaque_identity == 22 && hint.present_count == 0);
        assert(event.header.timestamp_ns != 0 && event.header.sequence == 1 && event.header.thread_id != 0);
    }) == 1);
    second->~CheapObserver();

    arc::CheapObserver fair;
    for (std::uint64_t i = 0; i < 64; ++i) fair.notify_creation(3, i, 1);
    std::thread other([&] { for (std::uint64_t i = 0; i < 64; ++i) fair.notify_creation(4, i, 1); });
    other.join();
    assert(fair.snapshot().claimed_slots_high_water == 2);
    std::uint64_t first_kind{}, second_kind{};
    assert(fair.drain([&](const arc::Event& event) {
        arc::CheapObserver::CreationHint hint;
        std::memcpy(&hint, event.payload.data(), sizeof(hint));
        first_kind = hint.kind;
    }, 16) == 16);
    assert(fair.drain([&](const arc::Event& event) {
        arc::CheapObserver::CreationHint hint;
        std::memcpy(&hint, event.payload.data(), sizeof(hint));
        second_kind = hint.kind;
    }, 16) == 16);
    assert(first_kind == 3 && second_kind == 4); // bounded calls rotate producers

    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 10'000; ++i) { observer.observe_present(16.0); observer.observe_submission(); }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    snapshot = observer.snapshot();
    assert(snapshot.presents == 10'000 && snapshot.submissions == 10'000);
    assert(snapshot.present_duration_us == 160'000'000);
    std::cout << "Cheap observer overflow/epochs/TLS PASS; diagnostic counter loop "
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us for 10000 iterations (no performance threshold)\n";
    volatile unsigned sink=0;
    auto meter_check=[&](bool enabled){arc::InterceptCpuMeter::enable(enabled);const auto begin=std::chrono::steady_clock::now();for(unsigned n=0;n<10000;++n){arc::InterceptCpuMeter::Scope hook(true);arc::original_cpu_call([&]{sink=n;});}return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count()/10000.;};
    const auto off=meter_check(false),on=meter_check(true);arc::InterceptCpuMeter::enable(false);
    std::cout<<"Diagnostic meter ns/call disabled="<<off<<" enabled="<<on<<" (no frame-cost claim)\n";

}
