#include "arc/compatibility_guard.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace arc;

namespace {
template <typename T>
Event event(EventType type, const T& payload, std::uint64_t sequence) {
    Event e{};
    e.header.type = type;
    e.header.payload_bytes = sizeof(T);
    e.header.sequence = sequence;
    e.header.timestamp_ns = sequence * 1000;
    std::memcpy(e.payload.data(), &payload, sizeof(T));
    return e;
}

ResourceGraph make_graph(ResourceAllocationKind allocation) {
    ResourceGraph graph;
    std::uint64_t seq = 1;
    for (ResourceId id = 1; id <= 20; ++id) {
        ResourceCreatePayload create{};
        create.resource = id;
        create.virtual_bytes = 4 * 1024 * 1024;
        create.allocation_bytes =
            allocation == ResourceAllocationKind::External ? 0 : 4 * 1024 * 1024;
        create.width = 1024;
        create.height = 1024;
        create.depth = 1;
        create.mip_levels = 8;
        create.array_layers = 1;
        create.kind = ResourceKind::Texture2D;
        create.allocation_kind = allocation;
        graph.consume(event(EventType::ResourceCreated, create, seq++));

        DescriptorWrittenPayload view{};
        view.descriptor = id + 1000;
        view.resource = id;
        view.type = ViewType::Srv;
        view.mip_count = 8;
        graph.consume(event(EventType::DescriptorWritten, view, seq++));
    }
    graph.analyze();
    return graph;
}
}

int main() {
    {
        ResourceGraph empty;
        const auto semantics = SceneUnderstandingModel::infer_all(empty);
        const auto d = CompatibilityGuard::evaluate(empty, semantics, {});
        assert(d.mode == CompatibilityMode::ObserveOnly);
        assert(!d.allow_quality);
        assert(!d.allow_residency);
    }

    {
        auto graph = make_graph(ResourceAllocationKind::Committed);
        const auto semantics = SceneUnderstandingModel::infer_all(graph);
        const auto d = CompatibilityGuard::evaluate(graph, semantics, {});
        assert(d.semantic_coverage >= 0.90);
        assert(d.mode == CompatibilityMode::FullControl);
        assert(d.allow_quality);
        assert(d.allow_residency);
    }

    {
        auto graph = make_graph(ResourceAllocationKind::External);
        const auto semantics = SceneUnderstandingModel::infer_all(graph);
        const auto d = CompatibilityGuard::evaluate(graph, semantics, {});
        assert(d.mode == CompatibilityMode::QualityOnly);
        assert(d.allow_quality);
        assert(!d.allow_residency);
        assert((d.reasons & CompatibilityExternalResidencyUnsafe) != 0);
    }

    {
        auto graph = make_graph(ResourceAllocationKind::Committed);
        const auto semantics = SceneUnderstandingModel::infer_all(graph);
        CompatibilityTelemetry telemetry{};
        telemetry.observation_failures = 1;
        const auto d = CompatibilityGuard::evaluate(graph, semantics, telemetry);
        assert(d.mode == CompatibilityMode::ObserveOnly);
        assert(!d.allow_quality);
        assert(!d.allow_residency);
    }

    std::cout << "compatibility-guard-tests: PASS\n";
    return 0;
}
