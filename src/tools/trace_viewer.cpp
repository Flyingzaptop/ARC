#include "arc/trace.hpp"
#include "arc/resource_graph.hpp"
#include <iostream>
#include <fstream>
#include <array>
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: arc-trace-viewer TRACE [summary.json]\n"; return 2; }
    auto trace = arc::TraceReader::inspect(argv[1]);
    arc::ResourceGraph graph;
    std::uint64_t peakCommitted{}, peakHeaps{}, peakBudget{}, peakUsage{}, created{}, destroyed{}, presents{}, signals{}, waits{};
    for (const auto& e : trace.events) {
        graph.consume(e);
        created += e.header.type == arc::EventType::ResourceCreated;
        destroyed += e.header.type == arc::EventType::ResourceDestroyed;
        presents += e.header.type == arc::EventType::Present;
        signals += e.header.type == arc::EventType::FenceSignal;
        waits += e.header.type == arc::EventType::FenceWait;
        peakCommitted = (std::max)(peakCommitted, graph.committed_bytes());
        peakHeaps = (std::max)(peakHeaps, graph.live_heap_bytes());
        if (auto budget = graph.latest_budget()) { peakBudget = (std::max)(peakBudget, budget->local_budget); peakUsage = (std::max)(peakUsage, budget->local_usage); }
    }
    graph.analyze();
    std::array<std::uint64_t, 5> kinds{}; std::array<std::uint64_t, 4> safety{}; std::uint64_t live{}, cold{};
    for (const auto& [id, r] : graph.resources()) {
        ++kinds[static_cast<unsigned>(r.description.kind)]; ++safety[static_cast<unsigned>(r.safety)];
        live += r.alive; cold += r.temperature == arc::Temperature::Cold;
    }
    auto write = [&](std::ostream& out) {
        out << "{\"schema\":1,\"trace_status\":" << static_cast<unsigned>(trace.status)
            << ",\"events\":" << trace.events.size() << ",\"created\":" << created << ",\"destroyed\":" << destroyed
            << ",\"live_at_shutdown\":" << live << ",\"peak_committed_bytes\":" << peakCommitted << ",\"peak_heap_bytes\":" << peakHeaps
            << ",\"peak_local_budget\":" << peakBudget << ",\"peak_local_usage\":" << peakUsage
            << ",\"submissions\":" << graph.submissions().size() << ",\"copies\":" << graph.copies().size()
            << ",\"presents\":" << presents << ",\"signals\":" << signals << ",\"waits\":" << waits
            << ",\"graph_errors\":" << graph.errors() << ",\"cold_resources\":" << cold << ",\"unknown_resources\":" << safety[0]
            << ",\"green_candidates\":" << safety[1] << ",\"yellow_resources\":" << safety[2] << ",\"red_resources\":" << safety[3]
            << ",\"largest_resources\":[";
        bool first = true;
        for (auto id : graph.largest_resources(10)) {
            if (!first) { out << ','; } first = false;
            const auto r = graph.find(id);
            out << "{\"id\":" << id << ",\"allocation_bytes\":" << r->description.allocation_bytes << ",\"logical_estimate_bytes\":" << r->description.virtual_bytes
                << ",\"kind\":" << static_cast<unsigned>(r->description.kind) << ",\"safety\":" << static_cast<unsigned>(r->safety) << ",\"view_evidence\":" << r->evidence << '}';
        }
        out << "]}\n";
    };
    write(std::cout);
    if (argc > 2) { std::ofstream out(argv[2]); write(out); if (!out) { return 1; } }
    return trace.status == arc::TraceReader::Status::Complete && graph.errors() == 0 ? 0 : 1;
}
