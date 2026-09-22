#include "arc/target_feedback.hpp"
#include "arc/compute_feedback_policy.hpp"
#include <limits>
int main(){
    arc::FrameTimePid mild,severe;
    auto m=mild.step(12,10,.25),h=severe.step(12,1.2,.25);
    if(!m.valid||h.output<=m.output)return 20;
    for(int i=0;i<400;++i)h=severe.step(12,1.2,.25);
    if(h.output!=1||h.integral>.001)return 21;
    h=severe.step(12,20,.25);if(h.output>=1||std::abs(h.derivative)>.001)return 22;
    h=severe.step(12,1.2,.25,0);if(h.output!=0||h.integral!=0)return 23;
    if(severe.step(-1,10,.25).valid)return 24;
    arc::FrameTimePid plant;double u=0;
    for(int i=0;i<800;++i){auto sample=plant.step(20-10*u,12,.25);u=sample.output;}
    if(std::abs((20-10*u)-12)>.4)return 25;
    plant.reset();if(plant.step(10,10,.25).output!=0)return 26;
    const arc::ComputeFeedbackTarget knobs[]{{1,8,true,true,true,true,true},{2,1,true,false,false,false,false}};
    auto full=arc::compute_feedback_policy(knobs,1,true);
    if(!full.valid()||full.compute.size()!=2||full.compute[0].x_rate!=4||full.compute[0].y_rate!=4||full.compute[0].mip_steps!=8||full.compute[0].sample_percent!=25||!full.compute[0].comparison_taps||!full.compute[0].zero_factor)return 27;
    if(!arc::compute_feedback_policy(knobs,0,true).compute.empty())return 28;
    if(arc::compute_feedback_policy(knobs,.3,true).find(2))return 29;
    auto balanced=arc::compute_feedback_policy(knobs,1,false);
    if(balanced.compute[0].x_rate>2||balanced.compute[0].y_rate>2||balanced.compute[0].mip_steps>4)return 30;
    const arc::ComputeFeedbackTarget marginal[]{{10,8,true,false,true,false,false},{11,3,true,false,false,false,false}};
    const auto distributed=arc::allocate_compute_budget(marginal,.85,true);
    if(!distributed.policy.find(11)||distributed.policy.find(10)->mip_steps==8)return 31;
    const auto impossible=arc::allocate_compute_budget(marginal,100,true);
    if(impossible.estimated_saved_ms>impossible.estimated_capacity_ms+1e-8||!impossible.policy.valid())return 32;
    arc::TargetFeedbackGate g;
    g.observe(18,16);if(g.reduce())return 1;
    g.observe(16,16);g.observe(18,16);if(g.reduce())return 2;
    g.observe(18,16);if(!g.reduce())return 3;
    g.reset();for(int i=0;i<2;++i)g.observe(10,16);if(g.recover())return 4;
    g.observe(10,16);if(!g.recover())return 5;
    g.observe(15,16);if(g.recover())return 6;
    if(arc::reject_feedback_change(10,15,16,true))return 7;
    if(!arc::reject_feedback_change(10,18,16,true))return 8;
    if(!arc::reject_feedback_change(10,11,16,false))return 9;
    if(arc::reject_feedback_change(10,9,16,false))return 10;
    if(!arc::reject_feedback_change(10,std::numeric_limits<double>::quiet_NaN(),16,false))return 11;
    g.observe(18,16);g.observe(-1,16);if(g.reduce()||g.recover())return 12;
}
