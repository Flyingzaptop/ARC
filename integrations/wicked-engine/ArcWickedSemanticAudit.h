#pragma once
// Evaluation-only catalog audited against pinned Wicked allocation owners.
// Never include in arc-core or pass its names/roles to inference/control.
#include "arc/resource_semantics.hpp"
#include <array>
#include <string_view>

namespace arc_wicked::audit {
enum class Family { ColorOutput, DepthAttachment, ShaderImage, ShaderBuffer, Upload, Readback, Count };
struct Entry { std::string_view name; Family family; };
inline constexpr auto catalog = std::to_array<Entry>({
    {"renderpath3D.rtMain",Family::ColorOutput},
    {"renderpath3D.rtMain_render",Family::ColorOutput},
    {"renderpath3D.rtPrimitiveID",Family::ColorOutput},
    {"renderpath3D.rtPrimitiveID_render",Family::ColorOutput},
    {"renderpath3D.rtParticleDistortion",Family::ColorOutput},
    {"renderpath3D.rtParticleDistortion_render",Family::ColorOutput},
    {"renderpath3D.rtPostprocess",Family::ColorOutput},
    {"renderpath3D.rtSceneCopy_tmp",Family::ColorOutput},
    {"shadowMapAtlas_Transparent",Family::ColorOutput},
    {"renderpath3D.depthBuffer_Main",Family::DepthAttachment},
    {"shadowMapAtlas",Family::DepthAttachment},
    {"envrenderingDepthBuffer",Family::DepthAttachment},
    {"renderpath3D.rtSceneCopy",Family::ShaderImage},
    {"renderpath3D.depthBuffer_Copy",Family::ShaderImage},
    {"renderpath3D.depthBuffer_Copy1",Family::ShaderImage},
    {"renderpath3D.rtGUIBlurredBackground[0]",Family::ShaderImage},
    {"renderpath3D.rtGUIBlurredBackground[1]",Family::ShaderImage},
    {"renderpath3D.rtGUIBlurredBackground[2]",Family::ShaderImage},
    {"renderpath3D.debugUAV",Family::ShaderImage},
    {"Scene::instanceBuffer",Family::ShaderBuffer},
    {"Scene::materialBuffer",Family::ShaderBuffer},
    {"Scene::geometryBuffer",Family::ShaderBuffer},
    {"Scene::skinningBuffer",Family::ShaderBuffer},
    {"Scene::textureStreamingFeedbackBuffer",Family::ShaderBuffer},
    {"Scene::instanceUploadBuffer",Family::Upload},
    {"Scene::materialUploadBuffer",Family::Upload},
    {"Scene::geometryUploadBuffer",Family::Upload},
    {"Scene::skinningUploadBuffer",Family::Upload},
    {"Scene::textureStreamingFeedbackBuffer_readback",Family::Readback},
    {"Scene::queryResultBuffer",Family::Readback},
});
inline int find(std::string_view name) noexcept {
    for (std::size_t i=0;i<catalog.size();++i) if(catalog[i].name==name) return static_cast<int>(i);
    return -1;
}
inline int predicted_family(arc::InferredResourceSemantic semantic) noexcept {
    using S=arc::InferredResourceSemantic;
    switch(semantic) {
    case S::RenderTarget: case S::TransientIntermediate: case S::PersistentHistory: return int(Family::ColorOutput);
    case S::DepthBuffer: case S::ShadowMap: return int(Family::DepthAttachment);
    case S::StorageTexture: return int(Family::ShaderImage);
    case S::StorageBuffer: case S::GeometryBuffer: return int(Family::ShaderBuffer);
    case S::UploadLikeBuffer: return int(Family::Upload);
    case S::ReadbackLikeBuffer: return int(Family::Readback);
    default: return -1;
    }
}
} // namespace arc_wicked::audit
