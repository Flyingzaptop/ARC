#pragma once
// Host-only benchmark adapter. ARC receives only ordinary DX12 API traffic.
#include <windows.h>
#include "arc_timeline.h"
#include "arc_async_writer.h"
#include <chrono>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstdlib>
namespace arc_bench {
using Clock=std::chrono::steady_clock;
using Api=DWORD(WINAPI*)(void*);
struct State {
    bool enabled{},ready{};
    unsigned tick{},warmup{120},frames{600};
    unsigned oracle_interval{};
    bool timed{},measuring{},finished{},phase_locked{},phase_comparable{true},holdout{};unsigned measurement_first_tick{};double actual_initialization_seconds{};unsigned measured_frames{};
    double initialization_seconds{},measurement_seconds{};
    Clock::time_point measurement_start;
    std::wstring output;
    std::ofstream rows;
    std::unique_ptr<AsyncWriter> writer;
    Clock::time_point frame_start,previous_start,previous_end,ready_start;
    double period_ms{},present_ms{},submit_ms{},wait_ms{},allocator_ms{};
    Api mode{},snapshot{},profile{},stop_profile{},stop_optimizer{};bool profile_requested{};
};
inline State& state(){static State s;return s;}
inline double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
inline std::wstring env(const wchar_t* name){wchar_t value[32768]{};GetEnvironmentVariableW(name,value,32768);return value;}
inline void initialize(){
    auto& s=state();s.output=env(L"ARC_BENCH_OUTPUT");s.enabled=!s.output.empty();if(!s.enabled)return;
    arc::timeline::enable(env(L"ARC_BENCH_TRACE")==L"1");
    const auto frames=env(L"ARC_BENCH_FRAMES");if(!frames.empty())s.frames=std::stoul(frames);
    const auto oracle=env(L"ARC_BENCH_ORACLE_INTERVAL");if(!oracle.empty())s.oracle_interval=std::stoul(oracle);
    if(s.oracle_interval&&s.oracle_interval<300)throw std::runtime_error("Oracle capture interval must be at least 300 frames");
    const auto seconds=env(L"ARC_BENCH_MEASUREMENT_SECONDS");if(!seconds.empty()){s.measurement_seconds=std::stod(seconds);s.initialization_seconds=std::stod(env(L"ARC_BENCH_INITIALIZATION_SECONDS"));s.timed=true;}
    if(s.timed&&(s.measurement_seconds<1||s.measurement_seconds>60||s.initialization_seconds<0||s.initialization_seconds>120))throw std::runtime_error("Bounded benchmark durations required");
    if(s.frames<1||(!s.timed&&s.frames>3600))throw std::runtime_error("Benchmark frame count 1..3600 required in frame-count mode");
    s.rows.open(s.output+L"/frames.jsonl");if(!s.rows)throw std::runtime_error("Cannot open benchmark output");
    s.writer.reset(new AsyncWriter(s.rows));s.holdout=env(L"ARC_BENCH_ROUTE")==L"holdout";s.phase_locked=s.timed&&s.initialization_seconds>=100;
    const auto dll=env(L"ARC_BENCH_DLL");if(dll.empty())return;
    const auto module=LoadLibraryW(dll.c_str());if(!module)throw std::runtime_error("Cannot load ARC benchmark DLL");
    auto init=reinterpret_cast<Api>(GetProcAddress(module,"ArcInitialize"));
    auto lean=reinterpret_cast<Api>(GetProcAddress(module,"ArcUseLeanMode"));
    s.mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalVrs"));
    s.snapshot=reinterpret_cast<Api>(GetProcAddress(module,"ArcSnapshot"));
    s.stop_optimizer=reinterpret_cast<Api>(GetProcAddress(module,"ArcStopOptimizer"));
    auto telemetry=s.output+L"/arc.json";
    if(!init||!lean||!s.mode||!s.snapshot||init(&telemetry[0])||lean(nullptr))throw std::runtime_error("ARC initialization failed");
    if(!env(L"ARC_AUTO_CONFIG").empty()){s.mode=nullptr;return;} // the automatic controller owns all policy/profile changes
    const auto experiment=env(L"ARC_BENCH_MODE");
    if(experiment==L"profile"){
        s.profile=reinterpret_cast<Api>(GetProcAddress(module,"ArcRequestGpuProfile"));
        s.stop_profile=reinterpret_cast<Api>(GetProcAddress(module,"ArcStopGpuProfile"));
        if(!s.profile||!s.stop_profile)throw std::runtime_error("GPU profile export unavailable");
    }else if(experiment.rfind(L"compute-",0)==0){
        s.mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalCompute"));
        auto rate=experiment.substr(8);
        if(rate.size()>4&&rate.compare(rate.size()-4,4,L"-hot")==0){rate.resize(rate.size()-4);rate+=L"|heaviest";
            s.profile=reinterpret_cast<Api>(GetProcAddress(module,"ArcRequestGpuProfile"));s.stop_profile=reinterpret_cast<Api>(GetProcAddress(module,"ArcStopGpuProfile"));if(!s.profile||!s.stop_profile)throw std::runtime_error("Compute discovery profiling unavailable");}
        if(!s.mode||rate.empty()||s.mode(&rate[0]))throw std::runtime_error("ARC compute mode refused");
    }else if(experiment!=L"observe"){
        wchar_t enable[]=L"2x2";if(s.mode(enable))throw std::runtime_error("ARC VRS mode refused");
    }
}
inline void before_frame(){auto& s=state();if(s.timed&&s.ready&&!s.measuring&&s.tick>=s.warmup){const auto elapsed=ms(Clock::now()-s.ready_start)*.001;if(elapsed>=s.initialization_seconds&&(!s.phase_locked||s.tick%600==0||elapsed>120)){s.phase_comparable=!s.phase_locked||(s.tick%600==0&&elapsed<=120);s.actual_initialization_seconds=elapsed;s.measurement_first_tick=s.tick;s.measuring=true;s.measurement_start=Clock::now();}}
if(s.profile&&!s.profile_requested&&s.ready&&s.tick==s.warmup){
    auto argument=L"16|"+s.output+L"/gpu-profile.json";if(s.profile(&argument[0]))throw std::runtime_error("GPU profile request refused");s.profile_requested=true;
}}
inline void finish(){auto& s=state();{std::ofstream phase(s.output+L"/measurement-phase.json");phase<<"{\"comparable\":"<<(s.phase_comparable?"true":"false")<<",\"initialization_seconds\":"<<s.actual_initialization_seconds<<",\"first_tick\":"<<s.measurement_first_tick<<",\"route\":\""<<(s.holdout?"holdout":"primary")<<"\"}\n";}if(s.writer&&!s.writer->finish())throw std::runtime_error("Benchmark telemetry lost or failed");arc::timeline::flush(s.output+L"/cpu-timeline.json");s.rows.close();if(s.stop_optimizer)s.stop_optimizer(nullptr);if(s.profile_requested){
    s.stop_profile(nullptr);const auto path=s.output+L"/gpu-profile.json";const auto until=GetTickCount64()+5000;
    while(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&GetTickCount64()<until)Sleep(10);
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("GPU profile publication timed out");
}if(s.mode){wchar_t off[]=L"off";if(s.mode(off))throw std::runtime_error("ARC rollback request failed");}if(s.snapshot)s.snapshot(nullptr);}
}
