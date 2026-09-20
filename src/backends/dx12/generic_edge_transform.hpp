#pragma once
#include "generic_shader_transform.hpp"
namespace arc::dx12::shader {
struct EdgeTransform {std::string ir;unsigned input_mask{};};
// Uniform per-macro-group classification from nine points in current inputs.
// The CPU admits only initialized, float-compatible views matching the output
// extent. High bit enables the guard; low bits select the proven input ranges.
EdgeTransform protect_input_edges(std::string_view controlled_ir,const Transform& contract);
}
