#pragma once
#include <algorithm>
#include <cmath>
namespace arc {
struct TargetFeedbackGate {
    unsigned slow{},spare{};
    void reset() noexcept {slow=spare=0;}
    void observe(double ms,double budget) noexcept {
        if(!std::isfinite(ms)||!std::isfinite(budget)||ms<=0||budget<=0){reset();return;}
        slow=ms>budget*1.03?std::min(2u,slow+1):0;
        spare=ms<budget*.9?std::min(3u,spare+1):0;
    }
    bool reduce() const noexcept{return slow>=2;}
    bool recover() const noexcept{return spare>=3;}
};
inline bool reject_feedback_change(double before,double after,double budget,bool recovering) noexcept {
    if(!std::isfinite(before)||!std::isfinite(after)||!std::isfinite(budget)||before<=0||after<=0||budget<=0)return true;
    // Restoring detail is allowed to be slower while still meeting the target.
    return recovering?after>budget*1.03:after>before+std::max(.1,before*.03);
}
}
