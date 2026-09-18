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

    const auto all=infer.classify_all(g);
    assert(all.size()==5);
    std::cout<<"resource-semantics-tests: PASS\n";
}
