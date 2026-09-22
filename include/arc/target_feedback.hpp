#pragma once
#include <algorithm>
#include <cmath>
namespace arc {
struct FrameTimePidSample {
    double error_ms{},filtered_ms{},proportional{},integral{},derivative{},output{};
    bool saturated{},valid{};
};
class FrameTimePid {
    double filtered_{},last_{},integral_{},derivative_{},output_{};
    bool initialized_{};
public:
    void reset(double applied=0) noexcept {filtered_=last_=derivative_=0;integral_=output_=std::clamp(applied,0.,1.);initialized_=false;}
    FrameTimePidSample step(double measured_ms,double target_ms,double dt,double maximum=1) noexcept {
        if(!std::isfinite(measured_ms)||!std::isfinite(target_ms)||!std::isfinite(dt)||!std::isfinite(maximum)||measured_ms<=0||target_ms<=0||dt<=0){reset();return {};}
        dt=std::clamp(dt,.001,1.);maximum=std::clamp(maximum,0.,1.);
        if(!initialized_){filtered_=last_=measured_ms;initialized_=true;}
        filtered_+=(measured_ms-filtered_)*(1-std::exp(-dt/.3));
        const double velocity=(filtered_-last_)/dt/target_ms;last_=filtered_;
        derivative_+=(velocity-derivative_)*(1-std::exp(-dt/.35));
        const double error_ms=filtered_-target_ms,normalized=error_ms/target_ms;
        if(std::abs(normalized)<=.02){integral_=std::min(output_,maximum);derivative_=0;output_=integral_;return {error_ms,filtered_,0,integral_,0,integral_,maximum==0,true};}
        const double p=.65*normalized,d=.03*derivative_;
        const double proposed=std::clamp(integral_+.25*normalized*dt,0.,maximum);
        const double raw=p+proposed+d;
        // Conditional integration: no windup beyond either actuator boundary.
        if(!((raw>maximum&&normalized>0)||(raw<0&&normalized<0)))integral_=proposed;
        integral_=std::clamp(integral_,0.,maximum);
        const double desired=std::clamp(p+integral_+d,0.,maximum);
        output_=std::min(output_,maximum);
        output_=std::clamp(desired,std::max(0.,output_-.25*dt),std::min(maximum,output_+2.*dt));
        return {error_ms,filtered_,p,integral_,d,output_,raw>=maximum||raw<=0,true};
    }
};

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
