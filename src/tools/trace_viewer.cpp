#include "arc/trace.hpp"
#include "arc/resource_graph.hpp"
#include <iostream>
#include <fstream>
#include <array>
#include <cstring>
template<class T> T decode(const arc::Event& e) {
    T p{}; if (e.header.payload_bytes == sizeof(T)) { std::memcpy(&p, e.payload.data(), sizeof(T)); } return p;
}
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: arc-trace-viewer TRACE [summary.json] [timeline.csv]\n"; return 2; }
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
    if (argc > 3) {
        std::ofstream timeline(argv[3]);
        timeline << "timestamp_ns,sequence,event,queue,command,resource,related_resource,value0,value1\n";
        for (const auto& e : trace.events) {
            auto row = [&](const char* label, std::uint64_t queue, std::uint64_t command, std::uint64_t resource, std::uint64_t related, std::uint64_t value0, std::uint64_t value1) {
                timeline << e.header.timestamp_ns << ',' << e.header.sequence << ',' << label << ',' << queue << ',' << command << ',' << resource << ',' << related << ',' << value0 << ',' << value1 << '\n';
            };
            switch (e.header.type) {
            case arc::EventType::MemoryBudgetSample: {
                auto p = decode<arc::MemoryBudgetPayload>(e); row("local_budget", 0, 0, 0, 0, p.local_budget, p.local_usage); row("nonlocal_budget", 0, 0, 0, 0, p.nonlocal_budget, p.nonlocal_usage); break;
            }
            case arc::EventType::QueueSubmit: { auto p = decode<arc::QueueSubmitPayload>(e); row("submit", p.queue, p.command, 0, 0, p.submission, 0); break; }
            case arc::EventType::FenceSignal: case arc::EventType::FenceWait: {
                auto p = decode<arc::FencePayload>(e); row(e.header.type == arc::EventType::FenceSignal ? "signal" : "wait", p.queue, 0, p.fence, 0, p.value, 0); break;
            }
            case arc::EventType::Present: { auto p = decode<arc::PresentPayload>(e); row("present", 0, 0, p.swapchain, 0, p.frame, static_cast<std::uint32_t>(p.result)); break; }
            case arc::EventType::Barrier: { auto p = decode<arc::BarrierPayload>(e); row("barrier", 0, p.command, p.resource, 0, p.before_state, p.after_state); break; }
            case arc::EventType::CopyResource: case arc::EventType::CopyBuffer: case arc::EventType::CopyTexture: case arc::EventType::ResolveSubresource: {
                auto p = decode<arc::CopyPayload>(e); row("copy_or_resolve_recorded", 0, p.command, p.source, p.destination, p.approximate_bytes, static_cast<unsigned>(e.header.type)); break;
            }
            default: break;
            }
        }
        if (!timeline) { return 1; }
        std::ofstream resources(std::string(argv[3]) + ".resources.csv");
        resources << "id,kind,heap,heap_offset,allocation_kind,allocation_bytes,logical_estimate_bytes,create_ns,destroy_ns,alive,reads,writes,safety,evidence,temperature,reuse_frames\n";
        for (const auto& [id, r] : graph.resources()) {
            const auto& d = r.description;
            resources << id << ',' << static_cast<unsigned>(d.kind) << ',' << d.heap << ',' << d.heap_offset << ',' << static_cast<unsigned>(d.allocation_kind) << ',' << d.allocation_bytes << ',' << d.virtual_bytes << ',' << r.create_timestamp_ns << ',' << r.destroy_timestamp_ns << ',' << r.alive << ',' << r.read_count << ',' << r.write_count << ',' << static_cast<unsigned>(r.safety) << ',' << r.evidence << ',' << static_cast<unsigned>(r.temperature) << ',' << r.reuse_interval_frames << '\n';
        }
        if (!resources) { return 1; }
    }
    return trace.status == arc::TraceReader::Status::Complete && graph.errors() == 0 ? 0 : 1;
}
