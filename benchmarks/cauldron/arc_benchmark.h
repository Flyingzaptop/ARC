#pragma once
// Host-only benchmark adapter. ARC receives only ordinary DX12 API traffic.
#include <windows.h>
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
    std::wstring output;
    std::ofstream rows;
    Clock::time_point frame_start,previous_start,previous_end,ready_start;
    double period_ms{},present_ms{},submit_ms{},wait_ms{},allocator_ms{};
    Api mode{},snapshot{};
};
inline State& state(){static State s;return s;}
inline double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
inline std::wstring env(const wchar_t* name){wchar_t value[32768]{};GetEnvironmentVariableW(name,value,32768);return value;}
inline void initialize(){
    auto& s=state();s.output=env(L"ARC_BENCH_OUTPUT");s.enabled=!s.output.empty();if(!s.enabled)return;
    const auto frames=env(L"ARC_BENCH_FRAMES");if(!frames.empty())s.frames=std::stoul(frames);
    if(s.frames<1||s.frames>1800)throw std::runtime_error("Benchmark frame count 1..1800 required");
    s.rows.open(s.output+L"/frames.jsonl");if(!s.rows)throw std::runtime_error("Cannot open benchmark output");
    const auto dll=env(L"ARC_BENCH_DLL");if(dll.empty())return;
    const auto module=LoadLibraryW(dll.c_str());if(!module)throw std::runtime_error("Cannot load ARC benchmark DLL");
    auto init=reinterpret_cast<Api>(GetProcAddress(module,"ArcInitialize"));
    auto lean=reinterpret_cast<Api>(GetProcAddress(module,"ArcUseLeanMode"));
    s.mode=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalVrs"));
    s.snapshot=reinterpret_cast<Api>(GetProcAddress(module,"ArcSnapshot"));
    auto telemetry=s.output+L"/arc.json";
    if(!init||!lean||!s.mode||!s.snapshot||init(&telemetry[0])||lean(nullptr))throw std::runtime_error("ARC initialization failed");
    wchar_t enable[]=L"2x2";if(s.mode(enable))throw std::runtime_error("ARC VRS mode refused");
}
inline void finish(){auto& s=state();s.rows.close();if(s.mode){wchar_t off[]=L"off";if(s.mode(off))throw std::runtime_error("ARC rollback request failed");}if(s.snapshot)s.snapshot(nullptr);}
}
