#include "generic_cpu_workers.hpp"
#include <mutex>

namespace arc::dx12::cpu_cost {
namespace {
struct Entry {HANDLE handle{};Kind kind{};std::uint64_t initial{};};
struct State {std::mutex mutex;std::array<Entry,16> entries;std::array<std::uint64_t,3> retired{};std::uint64_t failures{};};
State& state(){static auto* value=new State;return *value;}
thread_local unsigned registered_threads{};
bool read(const Entry& entry,std::uint64_t& ns){
    FILETIME created{},exited{},kernel{},user{};
    const auto ok=entry.kind==Kind::Thread?GetThreadTimes(entry.handle,&created,&exited,&kernel,&user):GetProcessTimes(entry.handle,&created,&exited,&kernel,&user);
    if(!ok)return false;
    const auto ticks=[](FILETIME t){return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    const auto total=(ticks(kernel)+ticks(user))*100;
    if(total<entry.initial)return false;ns=total-entry.initial;return true;
}
}
Registration::Registration(Kind kind,HANDLE process,HANDLE primary_thread)noexcept:placement_(kind==Kind::Thread?nullptr:process,primary_thread){
    auto& s=state();std::lock_guard lock(s.mutex);Entry entry;entry.kind=kind;
    const auto source=kind==Kind::Thread?GetCurrentThread():process;
    if(!source||!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&entry.handle,0,FALSE,DUPLICATE_SAME_ACCESS)){++s.failures;return;}
    if(kind==Kind::Thread&&!read(entry,entry.initial)){CloseHandle(entry.handle);++s.failures;return;}
    for(unsigned i=0;i<s.entries.size();++i)if(!s.entries[i].handle){s.entries[i]=entry;slot_=i;thread_=kind==Kind::Thread;if(thread_)++registered_threads;return;}
    CloseHandle(entry.handle);++s.failures;
}
Registration::~Registration(){
    if(slot_>=16)return;auto& s=state();std::lock_guard lock(s.mutex);auto& entry=s.entries[slot_];std::uint64_t elapsed{};
    if(read(entry,elapsed))s.retired[static_cast<unsigned>(entry.kind)]+=elapsed;else ++s.failures;
    CloseHandle(entry.handle);entry={};
    if(thread_)--registered_threads;
}
bool on_worker_thread()noexcept{return registered_threads!=0;}
Snapshot snapshot()noexcept{
    auto& s=state();std::lock_guard lock(s.mutex);Snapshot result;result.nanoseconds=s.retired;
    for(const auto& entry:s.entries)if(entry.handle){const auto kind=static_cast<unsigned>(entry.kind);++result.live[kind];std::uint64_t elapsed{};if(read(entry,elapsed))result.nanoseconds[kind]+=elapsed;else ++s.failures;}
    result.failures=s.failures;return result;
}
}
