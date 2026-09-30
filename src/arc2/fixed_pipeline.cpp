#include "shader_semantics.hpp"
#include "arc/arc2/runtime.hpp"
namespace arc::arc2 {
void describe_fixed_pipeline(ObjectId id,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d){
 FixedPipelineState f;const auto& r=d.RasterizerState;const auto& z=d.DepthStencilState;const auto& b=d.BlendState;
 f.raster_fill=r.FillMode==D3D12_FILL_MODE_SOLID?1:r.FillMode==D3D12_FILL_MODE_WIREFRAME?2:0;
 f.raster_cull=static_cast<unsigned>(r.CullMode);f.front_ccw=r.FrontCounterClockwise!=FALSE;f.depth_clip=r.DepthClipEnable!=FALSE;f.conservative_raster=r.ConservativeRaster!=D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;f.depth_bias=r.DepthBias;f.depth_bias_clamp=r.DepthBiasClamp;f.slope_scaled_depth_bias=r.SlopeScaledDepthBias;f.multisample_enable=r.MultisampleEnable!=FALSE;f.antialiased_line_enable=r.AntialiasedLineEnable!=FALSE;f.forced_sample_count=r.ForcedSampleCount;f.raster_known=f.raster_fill!=0&&f.raster_cull>=1&&f.raster_cull<=3;
 f.depth_enable=z.DepthEnable!=FALSE;f.depth_write=z.DepthWriteMask==D3D12_DEPTH_WRITE_MASK_ALL;f.depth_func=unsigned(z.DepthFunc);f.stencil_enable=z.StencilEnable!=FALSE;f.stencil_read_mask=z.StencilReadMask;f.stencil_write_mask=z.StencilWriteMask;
 auto stencil=[](const D3D12_DEPTH_STENCILOP_DESC& s){return FixedStencilFace{unsigned(s.StencilFailOp),unsigned(s.StencilDepthFailOp),unsigned(s.StencilPassOp),unsigned(s.StencilFunc)};};f.front_stencil=stencil(z.FrontFace);f.back_stencil=stencil(z.BackFace);f.depth_known=true;
 f.topology_type=unsigned(d.PrimitiveTopologyType);f.topology_known=f.topology_type>=1&&f.topology_type<=4;f.sample_mask=d.SampleMask;f.sample_count=d.SampleDesc.Count;
 f.alpha_to_coverage=b.AlphaToCoverageEnable!=FALSE;f.independent_blend=b.IndependentBlendEnable!=FALSE;for(unsigned i=0;i<8;++i){const auto&t=b.RenderTarget[i];f.targets[i]={t.BlendEnable!=FALSE,t.LogicOpEnable!=FALSE,unsigned(t.SrcBlend),unsigned(t.DestBlend),unsigned(t.BlendOp),unsigned(t.SrcBlendAlpha),unsigned(t.DestBlendAlpha),unsigned(t.BlendOpAlpha),unsigned(t.LogicOp),t.RenderTargetWriteMask};}f.blend_known=true;
 runtime().describe_pipeline_fixed(id,f);
}
}
