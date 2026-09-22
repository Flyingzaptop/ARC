#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <utility>
namespace arc {
inline std::uint64_t queue_union_ticks(std::vector<std::pair<std::uint64_t,std::uint64_t>> spans){
    std::sort(spans.begin(),spans.end());std::uint64_t first=0,last=0,total=0;
    for(const auto& [a,b]:spans){if(b<a)continue;if(a>last){total+=last-first;first=a;last=b;}else last=std::max(last,b);}return total+last-first;
}
enum class Bottleneck {Unknown,CpuThread,Gpu,Mixed};
struct GpuPermission {bool update{};double maximum{};};
inline GpuPermission gpu_permission(Bottleneck route,double frame_ms,double budget,double retained,bool available){
    if(route==Bottleneck::Gpu||route==Bottleneck::Mixed)return {true,available?1.:0.};
    if(std::isfinite(frame_ms)&&std::isfinite(budget)&&frame_ms>0&&budget>0&&frame_ms<budget*.9)return {true,std::clamp(retained,0.,1.)};
    return {false,std::clamp(retained,0.,1.)};
}
struct BottleneckEvidence {
    std::uint64_t sequence{};
    double frame_ms{},cpu_running_ms{},gpu_queue_busy_ms{},age_seconds{};
    bool cpu_valid{},gpu_valid{};double target_frame_ms{};
};
inline const char* bottleneck_name(Bottleneck b){switch(b){case Bottleneck::CpuThread:return "present_thread_cpu_pressure";case Bottleneck::Gpu:return "gpu_queue_pressure";case Bottleneck::Mixed:return "mixed_cpu_gpu_pressure";default:return "unknown";}}
class BottleneckRouter {
    std::uint64_t sequence_{};Bottleneck candidate_{},current_{};unsigned repeats_{};
public:
    void reset(){*this={};}
    Bottleneck observe(const BottleneckEvidence& e){
        if(!std::isfinite(e.age_seconds)||e.age_seconds<0||e.age_seconds>6||!std::isfinite(e.frame_ms)||e.frame_ms<=0){current_=candidate_=Bottleneck::Unknown;repeats_=0;return current_;}
        if(e.sequence==sequence_)return current_;
        sequence_=e.sequence;
        const double budget=std::isfinite(e.target_frame_ms)&&e.target_frame_ms>0?e.target_frame_ms*1.05:e.frame_ms;
        const bool cpu=e.cpu_valid&&std::isfinite(e.cpu_running_ms)&&e.cpu_running_ms>=std::min(e.frame_ms*.75,budget);
        const bool gpu=e.gpu_valid&&std::isfinite(e.gpu_queue_busy_ms)&&e.gpu_queue_busy_ms>=std::min(e.frame_ms*.8,budget);
        const auto next=cpu&&gpu?(e.cpu_running_ms>e.gpu_queue_busy_ms*1.25?Bottleneck::CpuThread:e.gpu_queue_busy_ms>e.cpu_running_ms*1.25?Bottleneck::Gpu:Bottleneck::Mixed):cpu?Bottleneck::CpuThread:gpu?Bottleneck::Gpu:Bottleneck::Unknown;
        if(next!=candidate_){candidate_=next;repeats_=1;}else repeats_=std::min(2u,repeats_+1);
        if(repeats_>=2)current_=next;
        return current_;
    }
};
}
