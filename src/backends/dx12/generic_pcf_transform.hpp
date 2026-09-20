#pragma once
#include <string>
#include <string_view>
namespace arc::dx12::shader {
struct ComparisonFilterTransform {std::string ir;unsigned groups{},samples_removed{};};
// Recognize equal-weight 5x5 comparison-filter reductions by their dataflow,
// not by shader identity or variable names. A control-CBV second uint4 selects
// nine taps; zero preserves the original calls and arithmetic exactly.
ComparisonFilterTransform sparse_comparison_filter(std::string_view controlled_ir);
}
