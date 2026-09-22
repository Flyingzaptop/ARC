#pragma once
#include <string>
#include <string_view>
namespace arc::dx12::shader {
struct MipTransform {std::string ir;unsigned samples{};};
// Explicit SampleLevel only. Loads, comparison samples, coordinates, samplers
// and descriptor identities are preserved. Runtime quality admission is separate.
// Static pixel-shader variant: Sample, SampleBias and SampleLevel, float32 only.
MipTransform bias_pixel_mips(std::string_view,unsigned half_steps);
MipTransform bias_explicit_mips(std::string_view);
}
