#pragma once
#include <string>
#include <string_view>
namespace arc::dx12::shader {
struct MipTransform {std::string ir;unsigned samples{};};
// Explicit SampleLevel only. Loads, comparison samples, coordinates, samplers
// and descriptor identities are preserved. Runtime quality admission is separate.
MipTransform bias_explicit_mips(std::string_view);
}
