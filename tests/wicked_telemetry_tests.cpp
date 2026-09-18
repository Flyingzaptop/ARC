#include "../integrations/wicked-engine/ArcWickedTelemetry.h"
#include <cassert>
#include <sstream>
#include <thread>
#include <vector>

int main() {
    arc_wicked::AccessMask read_then_write;
    read_then_write.observe(false);
    read_then_write.observe(true);
    assert(read_then_write.read && read_then_write.write);
    arc_wicked::AccessMask write_then_read;
    write_then_read.observe(true);
    write_then_read.observe(false);
    assert(write_then_read.read && write_then_read.write);
    arc_wicked::AccessMask only_read;
    only_read.observe(false);
    only_read.observe(false);
    assert(only_read.read && !only_read.write);
    arc_wicked::AccessMask only_write;
    only_write.observe(true);
    assert(!only_write.read && only_write.write);
    assert(arc_wicked::HookTiming::bucket(0) == 0);
    assert(arc_wicked::HookTiming::bucket(1) == 0);
    assert(arc_wicked::HookTiming::bucket(2) == 1);
    assert(arc_wicked::HookTiming::bucket(UINT64_MAX) == 63);
    arc_wicked::HookTiming timing;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i)
        workers.emplace_back([&] { for (unsigned j = 0; j < 10000; ++j) timing.record(8); });
    for (auto& worker : workers) worker.join();
    assert(timing.calls.load() == 40000);
    assert(timing.total_ns.load() == 320000);
    assert(timing.buckets[3].load() == 40000);
    std::ostringstream json;
    timing.write_json(json);
    assert(json.str().find("\"calls\":40000") != std::string::npos);
    { arc_wicked::HookTimer timer(timing); }
    assert(timing.calls.load() == 40001);
}
