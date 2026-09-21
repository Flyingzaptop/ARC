#pragma once
#include <span>
#include <cmath>
#include <limits>
#include <algorithm>
namespace arc {
struct TimingWindow {double mean{},variance{};std::size_t count{};};
inline TimingWindow timing_window(std::span<const double> values){
    TimingWindow result;for(double value:values){if(!std::isfinite(value)||value<=0)return {};const auto delta=value-result.mean;++result.count;result.mean+=delta/result.count;result.variance+=delta*(value-result.mean);}
    if(result.count>1)result.variance/=result.count-1;return result;
}
// Drift between both A windows and two standard errors of the A/B difference.
// This is measured uncertainty, not an invented percentage of baseline time.
inline double comparison_noise(const TimingWindow& a,const TimingWindow& b,const TimingWindow& c){
    if(a.count<2||b.count<2||c.count<2)return std::numeric_limits<double>::infinity();
    return std::max(std::abs(a.mean-c.mean),2*std::sqrt(a.variance/(4*a.count)+b.variance/b.count+c.variance/(4*c.count)));
}
}
