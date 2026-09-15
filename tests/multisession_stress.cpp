#include "arc/multi_session.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
static arc::Event event(std::uint64_t sequence, std::uint32_t producer, arc::EventType type = arc::EventType::TelemetrySample) {
    arc::Event e{}; e.header.sequence = sequence; e.header.thread_id = producer + 1; e.header.type = type;
    e.header.flags = 1; e.header.timestamp_ns = sequence; return e;
}
static bool push(arc::MultiSession& session, std::size_t producer, arc::Event e) {
    return session.ring(producer).try_emit(e);
}
int main(int argc, char** argv) {
    const unsigned rounds = argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 3;
    const std::uint64_t perRound = argc > 2 ? std::stoull(argv[2]) : 2000000;
    if (rounds < 2 || perRound < 2000000 || perRound % 4) { std::cerr << "Need >=2 rounds, >=2M events divisible by four\n"; return 2; }
    const auto path = std::filesystem::temp_directory_path() / "arc_adversarial_order.arcbin";
    for (unsigned round = 0; round < rounds; ++round) {
        // The test validates ordering, not overflow policy. Capacity holds the
        // entire maximum contribution of one producer so disk speed cannot
        // turn an ordering test into an expected-drop test.
        arc::MultiSession session(path, 4, static_cast<std::size_t>(perRound / 4 + 1));
        std::atomic<bool> firstReserved{}, releaseFirst{};
        std::vector<std::thread> producers;
        for (std::size_t producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&, producer] {
                for (std::uint64_t index = 0; index < perRound / 4; ++index) {
                    const auto sequence = session.sequence().fetch_add(1, std::memory_order_relaxed);
                    if (sequence == 1) {
                        firstReserved.store(true, std::memory_order_release);
                        while (!releaseFirst.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                    }
                    if (producer == 0 && index % 8192 == 0) { std::this_thread::sleep_for(100us); }
                    else if (producer == 2 && index % 97 == 0) { std::this_thread::yield(); }
                    else if (producer == 3 && index % 16384 == 0) { std::this_thread::sleep_for(50us); }
                    if (!push(session, producer, event(sequence, static_cast<std::uint32_t>(producer)))) { std::cerr << "Unexpected ring overflow\n"; std::terminate(); }
                }
            });
        }
        while (!firstReserved.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        std::this_thread::sleep_for(20ms); releaseFirst.store(true, std::memory_order_release);
        for (auto& producer : producers) { producer.join(); }
        session.finish();
        if (!session.complete() || session.dropped()) { std::cerr << "Incomplete multi-session\n"; return 1; }
        auto trace = arc::TraceReader::inspect(path);
        if (trace.status != arc::TraceReader::Status::Complete || trace.events.size() != perRound) { return 1; }
        for (std::size_t i = 0; i < trace.events.size(); ++i) {
            if (trace.events[i].header.sequence != i + 1) { std::cerr << "Order break at " << i << '\n'; return 1; }
        }
        std::cout << "round " << round + 1 << ": " << perRound << " monotonic events\n";
    }
    {
        arc::MultiSession session(path, 4, 64);
        std::atomic<unsigned> phase{};
        auto emit = [&](std::size_t producer, arc::EventType type, const auto& payload) {
            if (!session.emit(producer, type, payload)) { std::terminate(); }
        };
        std::thread a([&] {
            emit(0, arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = 10});
            emit(0, arc::EventType::CommandListCreated, arc::CommandListPayload{.command = 20});
            emit(0, arc::EventType::ResourceCreated, arc::ResourceCreatePayload{.resource = 30, .allocation_bytes = 4096});
            phase.store(1, std::memory_order_release);
        });
        std::thread b([&] { while (phase.load(std::memory_order_acquire) < 1) {} emit(1, arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = 40, .resource = 30, .type = arc::ViewType::Srv}); phase.store(2, std::memory_order_release); });
        std::thread c([&] {
            while (phase.load(std::memory_order_acquire) < 2) {}
            emit(2, arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = 20, .resource = 30});
            emit(2, arc::EventType::CommandListClosed, arc::CommandListPayload{.command = 20});
            emit(2, arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = 10, .command = 20, .submission = 1});
            phase.store(3, std::memory_order_release);
        });
        std::thread d([&] { while (phase.load(std::memory_order_acquire) < 3) {} emit(3, arc::EventType::ResourceDestroyed, arc::ResourceDestroyPayload{.resource = 30}); });
        a.join(); b.join(); c.join(); d.join(); session.finish();
        auto resource = session.graph().find(30);
        if (!session.complete() || session.graph().errors() || !resource || resource->alive || resource->read_count != 1 || !session.graph().find_view(40)) { std::cerr << "Causal graph mismatch\n"; return 1; }
    }
    std::filesystem::remove(path); std::filesystem::remove(path.string() + ".session.json");
    return 0;
}
