#pragma once
// Test harness only: the generic DLL receives no Wicked resources, labels or
// quality callbacks. The engine's previous ARC integration must be in off mode.
#include <windows.h>
#include <string>
#include <stdexcept>
namespace arc_generic_benchmark {
using Api=DWORD(WINAPI*)(void*);
inline Api snapshot{},profile{},stop{};
inline ULONGLONG started{},last_attempt{};inline bool requested{},automatic{};
inline std::wstring output;
inline std::wstring environment(const wchar_t* key){wchar_t value[32768]{};const auto n=GetEnvironmentVariableW(key,value,32768);if(n>=32768)throw std::runtime_error("Benchmark environment capacity");return {value,n};}
inline void initialize(){
    const auto dll=environment(L"ARC_BENCH_DLL");if(dll.empty())return;
    if(environment(L"ARC_WICKED_EXPERIMENT_MODE")!=L"off")throw std::runtime_error("Generic benchmark requires host quality actions off");
    output=environment(L"ARC_BENCH_OUTPUT");if(output.empty())throw std::runtime_error("Benchmark output required");
    automatic=!environment(L"ARC_AUTO_CONFIG").empty();
    auto module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)throw std::runtime_error("Generic DLL load");
    auto init=reinterpret_cast<Api>(GetProcAddress(module,"ArcInitialize"));auto lean=reinterpret_cast<Api>(GetProcAddress(module,"ArcUseLeanMode"));
    auto compute=reinterpret_cast<Api>(GetProcAddress(module,"ArcExperimentalCompute"));
    snapshot=reinterpret_cast<Api>(GetProcAddress(module,"ArcSnapshot"));profile=reinterpret_cast<Api>(GetProcAddress(module,"ArcRequestGpuProfile"));stop=reinterpret_cast<Api>(GetProcAddress(module,"ArcStopOptimizer"));
    auto path=output+L"/arc.json";auto mode=environment(L"ARC_BENCH_COMPUTE");if(mode.empty())mode=L"neutral|heaviest";
    if(!init||!lean||!compute||!snapshot||!profile||init(path.data())||lean(nullptr)||compute(mode.data()))throw std::runtime_error("Generic benchmark initialization");
    started=GetTickCount64();
}
inline void tick(){const auto now=GetTickCount64();if(profile&&!automatic&&!requested&&now-started>=8000&&now-last_attempt>=1000){last_attempt=now;auto path=L"16|"+output+L"/gpu-profile.json";requested=profile(path.data())==0;}}
inline void finish(){if(stop)stop(nullptr);if(snapshot)snapshot(nullptr);}
}
