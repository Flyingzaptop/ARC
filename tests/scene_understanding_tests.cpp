#include "arc/scene_understanding.hpp"

#include <cassert>
#include <iostream>

using namespace arc;

namespace {
std::uint32_t bit(ViewType type) {
    return 1U << static_cast<unsigned>(type);
}

ResourceRecord base_texture(ResourceId id) {
    ResourceRecord r{};
    r.description.resource = id;
    r.description.kind = ResourceKind::Texture2D;
    r.description.width = 1024;
    r.description.height = 1024;
    r.description.mip_levels = 8;
    r.description.array_layers = 1;
    r.alive = true;
    r.last_used_frame = 100;
    r.reuse_interval_frames = 1.0;
    return r;
}
}

int main() {
    {
        auto r = base_texture(1);
        r.evidence = bit(ViewType::Srv);
        r.read_count = 200;
        const auto e = SceneUnderstandingModel::infer_resource(r, 100);
        assert(e.semantic == ResourceSemantic::MaterialTexture);
        assert(e.confidence >= 0.55);
    }

    {
        auto r = base_texture(2);
        r.description.mip_levels = 1;
        r.description.array_layers = 4;
        r.evidence = bit(ViewType::Dsv) | bit(ViewType::Srv);
        r.read_count = 240;
        r.write_count = 40;
        const auto e = SceneUnderstandingModel::infer_resource(r, 100);
        assert(e.semantic == ResourceSemantic::ShadowMap);
        assert(e.confidence >= 0.55);
    }

    {
        auto r = base_texture(3);
        r.description.mip_levels = 1;
        r.evidence = bit(ViewType::Rtv);
        r.read_count = 5;
        r.write_count = 100;
        const auto e = SceneUnderstandingModel::infer_resource(r, 100);
        assert(e.semantic == ResourceSemantic::ColorTarget);
    }

    {
        ResourceRecord r{};
        r.description.resource = 4;
        r.description.kind = ResourceKind::Buffer;
        r.evidence = bit(ViewType::Cbv);
        r.read_count = 100;
        r.alive = true;
        const auto e = SceneUnderstandingModel::infer_resource(r, 100);
        assert(e.semantic == ResourceSemantic::ConstantBuffer);
    }

    {
        WorkloadTelemetrySample x{};
        x.frames = 16;
        x.gpu_ms = 18.0;
        x.target_ms = 16.7;
        x.dispatches = 16 * 500;
        x.resource_uses = 16 * 200;
        const auto s = SceneUnderstandingModel::infer_workload(x);
        assert(s.dominant == WorkloadClass::ComputeHeavy);
        assert(s.compute > 0.8);

        FrameBudgetSample frame{};
        frame.frame_ms = 18.0;
        frame.target_frame_ms = 16.7;
        const auto enriched = SceneUnderstandingModel::enrich_frame(frame, s);
        assert(enriched.lighting_pressure > 0.8);
    }

    {
        WorkloadTelemetrySample x{};
        x.frames = 16;
        x.gpu_ms = 20.0;
        x.target_ms = 16.7;
        x.resource_uses = 16 * 2500;
        x.descriptor_writes = 16 * 300;
        x.copies = 16 * 400;
        const auto s = SceneUnderstandingModel::infer_workload(x);
        assert(s.bandwidth > 0.9);
    }

    std::cout << "scene-understanding-tests: PASS\n";
    return 0;
}
