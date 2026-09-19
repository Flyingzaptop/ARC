#include "arc/frame_timing_window.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <stdexcept>

namespace arc {
FrameTimingWindow::FrameTimingWindow(std::uint64_t duration,std::size_t capacity)
    :duration_(duration),capacity_(capacity){
    if(!duration||duration>60'000'000'000ULL||!capacity||capacity>65536)
        throw std::invalid_argument("Frame window must be bounded to 60 seconds and 65536 intervals");
    intervals_.reserve(capacity);
}
void FrameTimingWindow::present(std::uint64_t time,std::uint64_t swap,bool visible){
    if(done())return;
    if(started_&&swap!=swapchain_){++ignored_;return;}
    if(!visible||!swap){reason_=Reason::Interrupted;return;}
    if(!started_){first_=last_=time;swapchain_=swap;started_=true;return;}
    if(time<=last_){reason_=Reason::InvalidTimestamp;return;}
    if(intervals_.size()==capacity_){reason_=Reason::Capacity;return;}
    intervals_.push_back(time-last_);last_=time;
    if(last_-first_>=duration_)reason_=Reason::Complete;
}
void FrameTimingWindow::expire()noexcept{
    if(!done())reason_=started_?Reason::Interrupted:Reason::NoPresents;
}
double FrameTimingWindow::mean_fps()const noexcept{
    return valid()&&last_>first_?double(intervals_.size())*1e9/double(last_-first_):0;
}
void FrameTimingWindow::write_json(std::ostream& out)const{
    static constexpr const char* names[]{"pending","complete","no_presents","interrupted","invalid_timestamp","capacity"};
    out<<std::setprecision(12)<<"{\"schema\":1,\"source\":\"cpu_present_return_intervals\",\"valid\":"<<(valid()?"true":"false")
       <<",\"reason\":\""<<names[static_cast<int>(reason_)]<<"\",\"requested_seconds\":"<<double(duration_)/1e9
       <<",\"elapsed_seconds\":"<<double(last_-first_)/1e9<<",\"swapchain\":"<<swapchain_<<",\"interval_count\":"<<intervals_.size()
       <<",\"other_swapchain_presents_ignored\":"<<ignored_<<",\"mean_fps\":";
    if(valid())out<<mean_fps();else out<<"null";
    if(valid()&&!intervals_.empty()){
        auto sorted=intervals_;std::sort(sorted.begin(),sorted.end());
        auto quantile=[&](double p){return double(sorted[static_cast<std::size_t>(std::ceil(p*double(sorted.size())))-1])/1e6;};
        const auto tail=std::max<std::size_t>(1,(sorted.size()+99)/100);
        const auto sum=std::accumulate(sorted.end()-tail,sorted.end(),0.0);
        out<<",\"p50_ms\":"<<quantile(.50)<<",\"p95_ms\":"<<quantile(.95)<<",\"p99_ms\":"<<quantile(.99)
           <<",\"one_percent_low_fps\":"<<double(tail)*1e9/sum;
    }
    out<<",\"gpu_frame_timing_available\":false,\"displayed_fps_verified\":false,\"dynamic_traversal_verified\":false,\"intervals_ns\":[";
    for(std::size_t i=0;i<intervals_.size();++i){if(i)out<<',';out<<intervals_[i];}out<<"]}";
}
}
