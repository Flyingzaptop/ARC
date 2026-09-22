#include "arc/target_feedback.hpp"
#include "arc/profile_retry.hpp"
#include "arc/bottleneck_router.hpp"
#include "arc/compute_feedback_policy.hpp"
#include <limits>
int main(){
    arc::ProfileRetry retry;if(retry.delay_seconds()!=2)return 101;
    retry.complete(false);if(retry.delay_seconds()!=4)return 102;
    retry.complete(false);retry.complete(false);
    if(retry.unavailable(29,false)||retry.unavailable(30,true)||!retry.unavailable(30,false))return 103;
    for(int i=0;i<20;++i)retry.complete(false);if(retry.delay_seconds()!=30)return 104;
    retry.complete(true);if(retry.failures()||retry.delay_seconds()!=2||retry.unavailable(100,false))return 105;

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
    if(!full.valid()||full.compute.size()!=2||full.compute[0].x_rate!=4||full.compute[0].y_rate!=4||full.compute[0].mip_steps!=8||full.compute[0].sample_percent!=1||!full.compute[0].comparison_taps||!full.compute[0].zero_factor)return 27;
    if(!arc::compute_feedback_policy(knobs,0,true).compute.empty())return 28;
    if(arc::compute_feedback_policy(knobs,.3,true).find(2))return 29;
    auto balanced=arc::compute_feedback_policy(knobs,1,false);
    if(balanced.compute[0].x_rate>2||balanced.compute[0].y_rate>2||balanced.compute[0].mip_steps>4)return 30;
    const arc::ComputeFeedbackTarget marginal[]{{10,8,true,false,true,false,false},{11,3,true,false,false,false,false}};
    const auto distributed=arc::allocate_compute_budget(marginal,.85,true);
    if(!distributed.policy.find(11)||distributed.policy.find(10)->mip_steps==8)return 31;
    const auto impossible=arc::allocate_compute_budget(marginal,100,true);
    if(impossible.estimated_saved_ms>impossible.estimated_capacity_ms+1e-8||!impossible.policy.valid())return 32;
    arc::BottleneckRouter router;
    arc::BottleneckEvidence gpu{1,16,2,15,0,true,true};
    if(router.observe(gpu)!=arc::Bottleneck::Unknown)return 33;
    if(router.observe(gpu)!=arc::Bottleneck::Unknown)return 34;
    gpu.sequence=2;if(router.observe(gpu)!=arc::Bottleneck::Gpu)return 35;
    arc::BottleneckEvidence cpu{3,16,15,3,0,true,true};router.observe(cpu);cpu.sequence=4;
    if(router.observe(cpu)!=arc::Bottleneck::CpuThread)return 36;
    cpu.age_seconds=7;if(router.observe(cpu)!=arc::Bottleneck::Unknown)return 37;
    // Waiting on GPU is low running CPU time; it must never classify as CPU.
    gpu.sequence=5;router.observe(gpu);gpu.sequence=6;if(router.observe(gpu)!=arc::Bottleneck::Gpu)return 38;
    arc::BottleneckEvidence mixed{7,16,15,15,0,true,true};router.observe(mixed);mixed.sequence=8;if(router.observe(mixed)!=arc::Bottleneck::Mixed)return 39;
    if(arc::queue_union_ticks({{10,20},{15,25},{10,20},{30,40}})!=25)return 40;
    if(arc::queue_union_ticks({{20,10}})!=0)return 41;
    router.reset();arc::BottleneckEvidence budget_cpu{1,24,12.5,3.4,0,true,true,10};router.observe(budget_cpu);budget_cpu.sequence=2;
    if(router.observe(budget_cpu)!=arc::Bottleneck::CpuThread)return 42;
    router.reset();arc::BottleneckEvidence unequal{1,24,14,8,0,true,true,5};router.observe(unequal);unequal.sequence=2;
    if(router.observe(unequal)!=arc::Bottleneck::CpuThread)return 43;
    auto hold=arc::gpu_permission(arc::Bottleneck::CpuThread,24,10,.5,true);if(hold.update)return 44;
    auto recovery=arc::gpu_permission(arc::Bottleneck::CpuThread,5,10,.5,true);if(!recovery.update||recovery.maximum!=.5)return 45;
    auto accelerate=arc::gpu_permission(arc::Bottleneck::Gpu,24,10,.5,true);if(!accelerate.update||accelerate.maximum!=1)return 46;
    arc::ComputePolicy fine;fine.pipeline=1;arc::ComputeFeedbackTarget all{1,1,false,true,false,true,false};
    for(unsigned percent=100;percent>1;--percent){if(fine.sample_percent!=percent||!arc::advance_compute_knob(fine,all,1,true))return 47;}
    if(fine.sample_percent!=1||arc::advance_compute_knob(fine,all,1,true))return 48;
    for(unsigned taps=24;taps>=1;--taps){if(!arc::advance_compute_knob(fine,all,3,true)||fine.comparison_taps!=taps)return 49;}
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
