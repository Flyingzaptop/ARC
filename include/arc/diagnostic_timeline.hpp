#pragma once
// Opt-in diagnostic recorder. No allocation, locks, formatting or IO in Scope.
// Flush only after owned producer threads have stopped.
#include <windows.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <string>
#include <fstream>
#include <memory>
namespace arc { namespace timeline {
struct Event {const char* name{};unsigned long tid{};unsigned long long frame{},begin{},end{},cpu100ns{};bool cpu_valid{};};
constexpr unsigned capacity=524288;
struct Recorder {std::atomic<bool> enabled{};std::atomic<unsigned> count{};std::atomic<unsigned long long> frame{};std::unique_ptr<Event[]> events;};
inline Recorder& recorder(){static Recorder value;return value;}
inline unsigned long long qpc(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
inline unsigned long long cpu(bool& valid){FILETIME c{},e{},k{},u{};valid=GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)!=0;return ((static_cast<unsigned long long>(k.dwHighDateTime)<<32)|k.dwLowDateTime)+((static_cast<unsigned long long>(u.dwHighDateTime)<<32)|u.dwLowDateTime);}
struct Scope {
    const char* name{};unsigned long long begin{},used{},frame{};bool valid{};
    explicit Scope(const char* label){auto& r=recorder();if(!r.enabled.load(std::memory_order_relaxed))return;name=label;frame=r.frame.load(std::memory_order_relaxed);used=cpu(valid);begin=qpc();}
    ~Scope(){if(!name)return;const auto end=qpc();bool ok{};const auto total=cpu(ok);auto& r=recorder();const auto i=r.count.fetch_add(1,std::memory_order_relaxed);if(i<capacity)r.events[i]={name,GetCurrentThreadId(),frame,begin,end,total-used,valid&&ok&&total>=used};}
};
inline void enable(bool value){auto& r=recorder();if(value&&!r.events)r.events.reset(new Event[capacity]);r.enabled=value;}
inline void flush(const std::wstring& path){auto& r=recorder();r.enabled=false;const auto count=r.count.load();if(!count)return;LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);std::ofstream f(path);f<<"{\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"qpc_frequency\":"<<frequency.QuadPart<<",\"dropped\":"<<(count>capacity?count-capacity:0)<<",\"events\":[";for(unsigned i=0;i<std::min(count,capacity);++i){const auto& e=r.events[i];if(i)f<<',';f<<"{\"phase\":\""<<e.name<<"\",\"tid\":"<<e.tid<<",\"frame\":"<<e.frame<<",\"begin_qpc\":"<<e.begin<<",\"end_qpc\":"<<e.end<<",\"cpu_100ns\":"<<e.cpu100ns<<",\"cpu_valid\":"<<(e.cpu_valid?"true":"false")<<'}';}f<<"]}\n";}
}
}
