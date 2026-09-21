#pragma once
#include <string_view>
#include <stdexcept>
namespace arc {
struct OptimizerQualityLimits {double ssim,mean,p99,worst,temporal;};
inline OptimizerQualityLimits optimizer_quality_limits(std::string_view profile){
    if(profile=="balanced")return {.98,.01,.04,.15,.02};
    if(profile=="aggressive")return {.94,.03,.12,.25,.06};
    throw std::invalid_argument("Unknown optimizer quality profile");
}
}
