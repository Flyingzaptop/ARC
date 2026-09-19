#include "arc/frame_timing_window.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
void check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
    arc::FrameTimingWindow steady;
    steady.present(100,7,true);
    for(unsigned i=1;i<=3000;++i)steady.present(100+std::uint64_t(i)*20'000'000,7,true);
    check(steady.valid()&&steady.intervals()==3000&&std::abs(steady.mean_fps()-50)<1e-9,"60s constant cadence");
    std::ostringstream json;steady.write_json(json);check(json.str().find("\"one_percent_low_fps\":50")!=std::string::npos,"tail statistic at constant cadence");
    arc::FrameTimingWindow irregular(100'000'000);
    irregular.present(0,4,true);irregular.present(10'000'000,4,true);irregular.present(11'000'000,8,true);
    irregular.present(20'000'000,4,true);irregular.present(100'000'000,4,true);
    check(irregular.valid()&&irregular.mean_fps()==30,"mean uses total interval duration, not average instantaneous FPS");
    std::ostringstream slow;irregular.write_json(slow);check(slow.str().find("\"one_percent_low_fps\":12.5")!=std::string::npos,"slow tail retains long frames");
    arc::FrameTimingWindow limit(100,2);limit.present(0,1,true);limit.present(1,1,true);limit.present(2,1,true);limit.present(100,1,true);
    check(limit.done()&&!limit.valid(),"capacity cannot truncate into a success");
    arc::FrameTimingWindow backwards;backwards.present(5,1,true);backwards.present(4,1,true);check(backwards.done()&&!backwards.valid(),"monotonic timestamps required");
    arc::FrameTimingWindow lost;lost.present(0,1,true);lost.present(1,1,false);check(lost.done()&&!lost.valid(),"occlusion/failure cannot count as fast frames");
    arc::FrameTimingWindow empty;empty.expire();check(empty.done()&&!empty.valid(),"no presents cannot produce FPS");
    arc::FrameTimingWindow short_run;short_run.present(0,1,true);short_run.present(10,1,true);short_run.expire();check(!short_run.valid(),"partial run cannot satisfy full window");
    arc::FrameTimingWindow background(100);background.present(0,1,true,false);background.present(100,1,true,true);
    std::ostringstream focus;background.write_json(focus);check(focus.str().find("\"foreground_only\":false")!=std::string::npos,"background throttling must remain visible in measurements");
    std::cout<<"Frame cadence: duration, slow-tail, multiple swapchains, capacity, occlusion and timeout PASS\n";
}
