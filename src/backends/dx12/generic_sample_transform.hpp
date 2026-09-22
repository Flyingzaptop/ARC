#pragma once
#include <string>
#include <string_view>
namespace arc::dx12::shader {
struct SampleTransform {std::string ir;unsigned loops{};};
// Fixed-count, single-block independent sample means only. Never RayQuery steps.
SampleTransform reduce_sample_means(std::string_view);
// Proved three-block outer means with a complete inner RayQuery.Proceed loop.
SampleTransform reduce_ray_means(std::string_view);
}
