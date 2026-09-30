#pragma once
#include "arc/arc2/ir.hpp"
#include <d3d12.h>
namespace arc::arc2 {
ObjectId register_shader_stage(D3D12_SHADER_BYTECODE,const char*);
void describe_root_signature(ObjectId,const void*,SIZE_T);
void describe_fixed_pipeline(ObjectId,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
void describe_graphics_pipeline(ObjectId,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
void describe_compute_pipeline(ObjectId,const D3D12_COMPUTE_PIPELINE_STATE_DESC&);
}
