#include "arc/resource_semantics.hpp"
#include "arc/resource_graph.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {
template<class T>
arc::Event event(arc::EventType type, const T& p, std::uint64_t seq=1) {
    arc::Event e{};
    e.header.type=type;
    e.header.sequence=seq;
    e.header.payload_bytes=sizeof(T);
    std::memcpy(e.payload.data(), &p, sizeof(T));
    return e;
}

void create(arc::ResourceGraph& g, arc::ResourceId id, arc::ResourceKind kind,
            std::uint64_t w, std::uint32_t h, std::uint16_t mips=1, std::uint16_t layers=1) {
    arc::ResourceCreatePayload p{};
    p.resource=id; p.kind=kind; p.width=w; p.height=h; p.depth=1;
    p.mip_levels=mips; p.array_layers=layers; p.allocation_bytes=static_cast<std::uint64_t>(w)*std::max(1u,h)*4;
    g.consume(event(arc::EventType::ResourceCreated,p,id));
}

void view(arc::ResourceGraph& g, arc::ResourceId id, arc::DescriptorId d, arc::ViewType type) {
    arc::DescriptorWrittenPayload p{}; p.descriptor=d; p.resource=id; p.type=type;
    g.consume(event(arc::EventType::DescriptorWritten,p,100+d));
}
}

int main() {
    arc::ResourceGraph g;
    arc::ResourceSemanticInferencer infer;

    create(g,1,arc::ResourceKind::Texture2D,2048,2048,1,4);
    view(g,1,11,arc::ViewType::Dsv);
    view(g,1,12,arc::ViewType::Srv);
    auto shadow=infer.classify(g,1);
    assert(shadow.semantic==arc::InferredResourceSemantic::ShadowMap);
    assert(shadow.confidence>=0.75f);

    create(g,2,arc::ResourceKind::Texture2D,1920,1080,1,1);
    view(g,2,21,arc::ViewType::Dsv);
    auto depth=infer.classify(g,2);
    assert(depth.semantic==arc::InferredResourceSemantic::DepthBuffer);

    create(g,3,arc::ResourceKind::Texture2D,1024,1024,10,1);
    view(g,3,31,arc::ViewType::Srv);
    // Usage-free material textures stay unknown until behavioral evidence exists.
    assert(infer.classify(g,3).semantic==arc::InferredResourceSemantic::Unknown);

    create(g,4,arc::ResourceKind::Texture2D,1280,720,1,1);
    view(g,4,41,arc::ViewType::Rtv);
    auto rt=infer.classify(g,4);
    assert(rt.semantic==arc::InferredResourceSemantic::RenderTarget);

    create(g,5,arc::ResourceKind::Texture2D,512,512,1,1);
    view(g,5,51,arc::ViewType::Uav);
    auto storage=infer.classify(g,5);
    assert(storage.semantic==arc::InferredResourceSemantic::StorageTexture);

    arc::ResourceSemanticFeatures storage_buffer{};
    storage_buffer.resource = 100;
    storage_buffer.kind = arc::ResourceKind::Buffer;
    storage_buffer.uav = true;
    storage_buffer.write_fraction = 0.95;
    storage_buffer.read_fraction = 0.05;
    storage_buffer.usage_count = 20;
    assert(infer.classify(storage_buffer).semantic ==
        arc::InferredResourceSemantic::StorageBuffer);

    arc::ResourceSemanticFeatures upload_like{};
    upload_like.resource = 101;
    upload_like.kind = arc::ResourceKind::Buffer;
    upload_like.cbv = true;
    upload_like.read_fraction = 0.95;
    upload_like.write_fraction = 0.05;
    upload_like.usage_count = 20;
    assert(infer.classify(upload_like).semantic ==
        arc::InferredResourceSemantic::UploadLikeBuffer);

    arc::ResourceSemanticFeatures readback_like{};
    readback_like.resource = 102;
    readback_like.kind = arc::ResourceKind::Buffer;
    readback_like.read_fraction = 0.02;
    readback_like.write_fraction = 0.98;
    readback_like.usage_count = 20;
    assert(infer.classify(readback_like).semantic ==
        arc::InferredResourceSemantic::ReadbackLikeBuffer);

    arc::ResourceSemanticFeatures geometry{};
    geometry.resource = 103;
    geometry.kind = arc::ResourceKind::Buffer;
    geometry.srv = true;
    geometry.read_fraction = 0.98;
    geometry.write_fraction = 0.02;
    geometry.usage_count = 20;
    assert(infer.classify(geometry).semantic ==
        arc::InferredResourceSemantic::GeometryBuffer);

    arc::ResourceCreatePayload external{};
    external.resource = 6;
    external.kind = arc::ResourceKind::Texture2D;
    external.allocation_kind = arc::ResourceAllocationKind::External;
    external.width = 640;
    external.height = 480;
    external.depth = 1;
    external.array_layers = 1;
    external.sample_count = 1;
    g.consume(event(arc::EventType::ResourceCreated, external, 600));
    const auto external_features = infer.extract(g, 6);
    assert(external_features.allocation_bytes == 640ull * 480ull * 4ull);

    arc::ResourceSemanticFeatures native_shadow{};
    native_shadow.resource = 104;
    native_shadow.kind = arc::ResourceKind::Texture2D;
    native_shadow.width = 2048;
    native_shadow.height = 2048;
    native_shadow.array_layers = 4;
    native_shadow.resource_flags = 0x2u; // D3D12 allow-depth-stencil capability
    native_shadow.srv = true;
    native_shadow.read_fraction = 0.35;
    native_shadow.write_fraction = 0.65;
    native_shadow.usage_count = 20;
    assert(infer.classify(native_shadow).semantic ==
        arc::InferredResourceSemantic::ShadowMap);

    arc::ResourceSemanticFeatures native_rt{};
    native_rt.resource = 105;
    native_rt.kind = arc::ResourceKind::Texture2D;
    native_rt.width = 1920;
    native_rt.height = 1080;
    native_rt.resource_flags = 0x1u; // D3D12 allow-render-target capability
    native_rt.read_fraction = 0.60;
    native_rt.write_fraction = 0.40;
    native_rt.usage_count = 20;
    assert(infer.classify(native_rt).semantic ==
        arc::InferredResourceSemantic::RenderTarget);

    const auto all=infer.classify_all(g);
    assert(all.size()==6);
    std::cout<<"resource-semantics-tests: PASS\n";
}
