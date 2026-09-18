#include "arc/scene_understanding.hpp"

#include <cassert>
#include <cstring>
#include <cmath>
#include <iostream>

namespace {

template<class T>
arc::Event event(arc::EventType type, const T& payload, std::uint64_t sequence = 1) {
    arc::Event out{};
    out.header.type = type;
    out.header.sequence = sequence;
    out.header.payload_bytes = sizeof(T);
    std::memcpy(out.payload.data(), &payload, sizeof(T));
    return out;
}

void create(
    arc::ResourceGraph& graph,
    arc::ResourceId id,
    arc::ResourceKind kind,
    std::uint64_t width,
    std::uint32_t height,
    std::uint16_t mips = 1,
    std::uint16_t layers = 1,
    std::uint64_t bytes = 0)
{
    arc::ResourceCreatePayload payload{};
    payload.resource = id;
    payload.kind = kind;
    payload.width = width;
    payload.height = height;
    payload.depth = 1;
    payload.mip_levels = mips;
    payload.array_layers = layers;
    payload.allocation_bytes =
        bytes ? bytes : width * static_cast<std::uint64_t>(height ? height : 1) * 4;
    graph.consume(event(arc::EventType::ResourceCreated, payload, id));
}

void view(
    arc::ResourceGraph& graph,
    arc::ResourceId id,
    arc::DescriptorId descriptor,
    arc::ViewType type)
{
    arc::DescriptorWrittenPayload payload{};
    payload.descriptor = descriptor;
    payload.resource = id;
    payload.type = type;
    graph.consume(event(arc::EventType::DescriptorWritten, payload, 100 + descriptor));
}

void queue(
    arc::ResourceGraph& graph,
    arc::QueueId id,
    arc::QueueClass type = arc::QueueClass::Graphics)
{
    arc::QueueCreatePayload payload{};
    payload.queue = id;
    payload.type = type;
    graph.consume(event(arc::EventType::CommandQueueCreated, payload, 500 + id));
}

void present(arc::ResourceGraph& graph, arc::FrameId frame) {
    arc::PresentPayload payload{};
    payload.frame = frame;
    graph.consume(event(arc::EventType::Present, payload, 1000 + frame));
}

void use(
    arc::ResourceGraph& graph,
    arc::CommandId command,
    arc::QueueId queue_id,
    arc::ResourceId resource,
    bool write,
    std::uint64_t sequence,
    std::uint64_t draws = 0,
    std::uint64_t indexed_draws = 0,
    std::uint64_t dispatches = 0,
    std::uint64_t indirect = 0,
    std::uint64_t draw_items = 0,
    std::uint64_t dispatch_groups = 0)
{
    arc::CommandListPayload list{};
    list.command = command;
    graph.consume(event(arc::EventType::CommandListCreated, list, sequence));

    arc::ResourceUsePayload usage{};
    usage.command = command;
    usage.resource = resource;
    usage.write = write ? 1u : 0u;
    graph.consume(event(arc::EventType::ResourceUse, usage, sequence + 1));

    arc::CountersPayload counters{};
    counters.command = command;
    counters.draws = draws;
    counters.indexed_draws = indexed_draws;
    counters.dispatches = dispatches;
    counters.indirect = indirect;
    counters.draw_items = draw_items;
    counters.dispatch_groups = dispatch_groups;
    graph.consume(event(arc::EventType::CommandCounters, counters, sequence + 2));

    graph.consume(event(arc::EventType::CommandListClosed, list, sequence + 3));

    arc::QueueSubmitPayload submit{};
    submit.queue = queue_id;
    submit.command = command;
    graph.consume(event(arc::EventType::QueueSubmit, submit, sequence + 4));
}

arc::ResourceGraph make_material_scene(std::uint64_t scale = 1) {
    arc::ResourceGraph graph;
    queue(graph, 1);
    create(graph, 1, arc::ResourceKind::Texture2D, 1024 * scale, 1024, 10);
    view(graph, 1, 11, arc::ViewType::Srv);
    create(graph, 2, arc::ResourceKind::Buffer, 4 * 1024 * 1024, 1, 1, 1, 4 * 1024 * 1024);
    view(graph, 2, 21, arc::ViewType::Srv);
    create(graph, 3, arc::ResourceKind::Texture2D, 1920, 1080);
    view(graph, 3, 31, arc::ViewType::Rtv);

    for (arc::FrameId frame = 1; frame <= 8; ++frame) {
        present(graph, frame);
        use(graph, 100 + frame * 3 + 0, 1, 1, false, 2000 + frame * 20 + 0);
        use(graph, 100 + frame * 3 + 1, 1, 2, false, 2000 + frame * 20 + 4);
        use(graph, 100 + frame * 3 + 2, 1, 3, true, 2000 + frame * 20 + 8);
    }
    return graph;
}

arc::ResourceGraph make_shadow_scene() {
    arc::ResourceGraph graph;
    queue(graph, 1);
    create(graph, 10, arc::ResourceKind::Texture2D, 2048, 2048, 1, 4);
    view(graph, 10, 101, arc::ViewType::Dsv);
    view(graph, 10, 102, arc::ViewType::Srv);
    create(graph, 11, arc::ResourceKind::Texture2D, 1024, 1024, 1, 6);
    view(graph, 11, 111, arc::ViewType::Dsv);
    view(graph, 11, 112, arc::ViewType::Srv);
    create(graph, 12, arc::ResourceKind::Texture2D, 1920, 1080);
    view(graph, 12, 121, arc::ViewType::Rtv);

    for (arc::FrameId frame = 1; frame <= 8; ++frame) {
        present(graph, frame);
        use(graph, 300 + frame * 3 + 0, 1, 10, frame % 3 != 0, 4000 + frame * 20 + 0);
        use(graph, 300 + frame * 3 + 1, 1, 11, frame % 2 == 0, 4000 + frame * 20 + 4);
        use(graph, 300 + frame * 3 + 2, 1, 12, true, 4000 + frame * 20 + 8);
    }
    return graph;
}

arc::ResourceGraph make_storage_scene() {
    arc::ResourceGraph graph;
    queue(graph, 1);
    queue(graph, 2, arc::QueueClass::Compute);
    create(graph, 20, arc::ResourceKind::Texture2D, 960, 540);
    view(graph, 20, 201, arc::ViewType::Uav);
    create(graph, 21, arc::ResourceKind::Texture2D, 960, 540);
    view(graph, 21, 211, arc::ViewType::Uav);
    create(graph, 22, arc::ResourceKind::Texture2D, 1920, 1080);
    view(graph, 22, 221, arc::ViewType::Rtv);
    view(graph, 22, 222, arc::ViewType::Srv);

    for (arc::FrameId frame = 1; frame <= 8; ++frame) {
        present(graph, frame);
        use(graph, 500 + frame * 3 + 0, 2, 20, true, 6000 + frame * 20 + 0);
        use(graph, 500 + frame * 3 + 1, 2, 21, true, 6000 + frame * 20 + 4);
        use(graph, 500 + frame * 3 + 2, 1, 22, frame % 2 == 0, 6000 + frame * 20 + 8);
    }
    return graph;
}

} // namespace

int main() {
    arc::SceneUnderstandingConfig config{};
    config.active_window_frames = 12;
    config.same_scene_distance = 0.22F;
    config.cluster_distance = 0.22F;
    arc::SceneUnderstandingInferencer infer(config);

    const auto material_a = infer.summarize(make_material_scene());
    const auto material_b = infer.summarize(make_material_scene(2));
    const auto shadows = infer.summarize(make_shadow_scene());
    const auto storage = infer.summarize(make_storage_scene());

    assert(material_a.active_resources == 3);
    assert(material_a.known_resources == 3);
    assert(material_a.coverage > 0.99F);
    assert(shadows.resource_fractions[
        static_cast<std::size_t>(arc::InferredResourceSemantic::ShadowMap)] > 0.60F);
    assert(storage.resource_fractions[
        static_cast<std::size_t>(arc::InferredResourceSemantic::StorageTexture)] > 0.60F);

    const auto same = infer.compare(material_a, material_b);
    const auto different_shadow = infer.compare(material_a, shadows);
    const auto different_storage = infer.compare(material_a, storage);
    assert(same.same_scene);
    assert(same.distance < different_shadow.distance);
    assert(same.distance < different_storage.distance);

    arc::SceneSemanticClusterer clusters(config);
    const auto a1 = clusters.observe(material_a);
    const auto s1 = clusters.observe(shadows);
    const auto c1 = clusters.observe(storage);
    const auto a2 = clusters.observe(material_b);
    assert(a1.created);
    assert(std::isfinite(a1.distance));
    assert(a1.distance == 1.0F);
    assert(s1.created);
    assert(c1.created);
    assert(!a2.created);
    assert(a2.cluster == a1.cluster);
    assert(clusters.cluster_count() == 3);

    // Identical resources with radically different command intensity must no
    // longer collapse to one scene identity. This models cases such as a
    // heavily instanced scene sharing the same material/resource set.
    arc::ResourceGraph light_workload;
    arc::ResourceGraph heavy_workload;
    queue(light_workload, 1);
    queue(heavy_workload, 1);
    create(light_workload, 40, arc::ResourceKind::Texture2D, 1024, 1024, 8);
    create(heavy_workload, 40, arc::ResourceKind::Texture2D, 1024, 1024, 8);
    view(light_workload, 40, 401, arc::ViewType::Srv);
    view(heavy_workload, 40, 401, arc::ViewType::Srv);
    for (arc::FrameId frame = 1; frame <= 8; ++frame) {
        present(light_workload, frame);
        present(heavy_workload, frame);
        use(light_workload, 2000 + frame, 1, 40, false, 20000 + frame * 10, 8, 0, 0, 0, 800);
        use(heavy_workload, 3000 + frame, 1, 40, false, 30000 + frame * 10, 8, 0, 0, 0, 800 * 65000ull);
    }
    const auto light_signature = infer.summarize(light_workload);
    const auto heavy_signature = infer.summarize(heavy_workload);
    assert(light_signature.draw_calls_per_frame > 0.0F);
    assert(std::abs(heavy_signature.draw_calls_per_frame - light_signature.draw_calls_per_frame) < 0.001F);
    assert(heavy_signature.draw_items_per_frame > light_signature.draw_items_per_frame * 10000.0F);
    const auto workload_difference = infer.compare(light_signature, heavy_signature);
    assert(!workload_difference.same_scene);
    assert(workload_difference.distance > config.same_scene_distance);

    arc::ResourceGraph stale_graph = make_storage_scene();
    present(stale_graph, 100);
    const auto stale = infer.summarize(stale_graph);
    assert(stale.active_resources == 0);

    arc::ResourceGraph window_graph = make_storage_scene();
    const auto checkpoint = infer.checkpoint(window_graph);
    present(window_graph, 100);
    const auto idle_window = infer.summarize(window_graph, checkpoint);
    assert(idle_window.active_resources == 0);
    assert(idle_window.coverage == 0.0F);
    assert(!infer.compare(material_a, idle_window).same_scene);
    const auto empty_cluster = clusters.observe(idle_window);
    assert(empty_cluster.cluster == 0);
    assert(!empty_cluster.created);

    use(window_graph, 900, 1, 22, false, 10000);
    use(window_graph, 901, 1, 22, true, 10010);
    use(window_graph, 902, 1, 22, true, 10020);
    const auto isolated_window = infer.summarize(window_graph, checkpoint);
    assert(isolated_window.active_resources == 1);
    assert(isolated_window.known_resources == 1);
    assert(isolated_window.resource_fractions[
        static_cast<std::size_t>(arc::InferredResourceSemantic::TransientIntermediate)] > 0.99F);

    arc::ResourceGraph queue_window;
    queue(queue_window, 1);
    queue(queue_window, 2, arc::QueueClass::Compute);
    create(queue_window, 30, arc::ResourceKind::Texture2D, 512, 512);
    view(queue_window, 30, 301, arc::ViewType::Uav);
    present(queue_window, 1);
    use(queue_window, 1000, 1, 30, true, 12000);
    use(queue_window, 1001, 2, 30, true, 12010);

    const auto queue_checkpoint = infer.checkpoint(queue_window);
    present(queue_window, 2);
    use(queue_window, 1002, 1, 30, true, 12020);
    const auto single_queue_window = infer.summarize(queue_window, queue_checkpoint);
    assert(single_queue_window.active_resources == 1);
    assert(single_queue_window.multi_queue_fraction == 0.0F);

    const auto queue_checkpoint2 = infer.checkpoint(queue_window);
    present(queue_window, 3);
    use(queue_window, 1003, 1, 30, true, 12030);
    use(queue_window, 1004, 2, 30, true, 12040);
    const auto multi_queue_window = infer.summarize(queue_window, queue_checkpoint2);
    assert(multi_queue_window.active_resources == 1);
    assert(multi_queue_window.multi_queue_fraction > 0.99F);

    arc::ResourceGraph reuse_window;
    queue(reuse_window, 1);
    create(reuse_window, 31, arc::ResourceKind::Texture2D, 1280, 720);
    view(reuse_window, 31, 311, arc::ViewType::Rtv);
    view(reuse_window, 31, 312, arc::ViewType::Srv);
    present(reuse_window, 1);
    use(reuse_window, 1100, 1, 31, true, 13000);
    present(reuse_window, 21);
    use(reuse_window, 1101, 1, 31, true, 13010);
    present(reuse_window, 41);
    use(reuse_window, 1102, 1, 31, true, 13020);

    const auto reuse_checkpoint = infer.checkpoint(reuse_window);
    for (arc::FrameId frame : {43ull, 45ull, 47ull, 49ull}) {
        present(reuse_window, frame);
        use(reuse_window, 1200 + frame, 1, 31, true, 14000 + frame * 10);
    }
    const auto dense_reuse_window = infer.summarize(reuse_window, reuse_checkpoint);
    assert(dense_reuse_window.active_resources == 1);
    assert(dense_reuse_window.known_resources == 1);
    assert(dense_reuse_window.resource_fractions[
        static_cast<std::size_t>(arc::InferredResourceSemantic::PersistentHistory)] > 0.99F);

    std::cout << "scene-understanding-tests: PASS\n";
}
