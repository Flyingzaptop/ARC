#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace arc::dx12::shader {
struct ResourceContract {
    unsigned resource_class{}, range_id{}, shader_register{}, space{}, count{}, kind{};
};
struct Transform {
    bool admitted{};
    std::string reason;
    std::string ir;
    std::array<unsigned, 3> threads{};
    std::vector<ResourceContract> resources;
    unsigned stores{};
    unsigned comparison_filter_groups{};
    unsigned zero_factor_regions{};
    unsigned edge_input_mask{};
    unsigned mip_samples{};
    bool execution_marker{};
    unsigned execution_marker_range{};
    // UINT32_MAX for static diagnostic variants; otherwise a new b0 binding in
    // this previously unused space. The root signature must explicitly bind it.
    unsigned control_space{UINT32_MAX};
};
// This proves a shader-local property ONLY. Admission at execution additionally
// requires complete physical bindings, no input/output aliasing and quality
// evidence. It never authorizes a dispatch based on reflection alone.
Transform coarse_compute(std::string_view dxil_ir, unsigned x_rate, unsigned y_rate, bool runtime_control = false,
    unsigned requested_control_space = UINT32_MAX, bool execution_marker = false);
// System DXBC converter output is driver-oriented: binding arrays lack their
// public LLVM array types and unused private op declarations remain. Normalize
// that representation only; callers must still assemble and validate the result.
std::string normalize_converted_dxil(std::string_view);
}
