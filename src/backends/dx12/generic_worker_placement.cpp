#include "generic_worker_placement.hpp"
#include "arc/worker_placement_plan.hpp"
#include "arc/worker_placement_search.hpp"
#include <array>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <string>
#include <ostream>
#include <iomanip>
#include <atomic>
#include <memory>
#include <numeric>

namespace arc::dx12::placement {
namespace {
using Mode=arc::WorkerMode;
const char* name(Mode m){switch(m){case Mode::Prefer:return "prefer";case Mode::Core:return "core";case Mode::Partition:return "partition";default:return "normal";}}
void check(BOOL ok){if(!ok)throw std::runtime_error("CPU placement Win32 error "+std::to_string(GetLastError()));}
std::vector<ULONG> sets(HANDLE handle,bool process){
    ULONG count=0;auto get=[&](ULONG* values,ULONG n){return process?GetProcessDefaultCpuSets(handle,values,n,&count):GetThreadSelectedCpuSets(handle,values,n,&count);};
    if(!get(nullptr,0)&&GetLastError()!=ERROR_INSUFFICIENT_BUFFER)check(FALSE);
    if(count>4096)throw std::runtime_error("CPU set capacity");std::vector<ULONG> values(count);if(count){check(get(values.data(),count));values.resize(count);}std::sort(values.begin(),values.end());return values;
}
void set_sets(HANDLE h,bool process,const std::vector<ULONG>& values){check(process?SetProcessDefaultCpuSets(h,values.empty()?nullptr:values.data(),static_cast<ULONG>(values.size())):SetThreadSelectedCpuSets(h,values.empty()?nullptr:values.data(),static_cast<ULONG>(values.size())));}
struct Entry {HANDLE handle{},primary{};bool process{};std::vector<ULONG> original;PROCESSOR_NUMBER ideal{};bool ideal_known{};};
struct State {
    std::mutex mutex;bool initialized{},supported{},allow_partition{},partition_owned{},conflict{},rollback_pending{};
    DWORD_PTR original_affinity{};
    Mode mode{};arc::WorkerPlacementPlan plan;std::vector<ULONG> original_game,installed_game;std::array<Entry,32> entries;
    std::uint64_t transitions{},failures{};std::string error;
    std::atomic<unsigned> published_mode{};std::array<std::atomic<std::uint64_t>,4> presents{};
    std::atomic<bool> adapting{};std::unique_ptr<arc::WorkerPlacementSearch> search;
    std::uint64_t workload_epoch{},workload_pipeline{},windows{},resets{};bool epoch_known{};
    std::uint64_t past_accepted{},past_rejected{},past_unstable{};
    std::uint32_t sample_seed{},sample_rng{};
    double last_mean{},last_p95{},last_p99{};
};
State& state(){static auto* value=new State;return *value;}
struct Samples {std::mutex mutex;std::array<double,120> frames{};unsigned count{},settle{240};void* swap{};LARGE_INTEGER last{},activity{};double frequency{};std::atomic<bool> dropped{};};
Samples& samples(){static auto* value=new Samples;return *value;}
void reset_samples(unsigned settle){auto& sample=samples();std::lock_guard lock(sample.mutex);sample.count=0;sample.settle=settle;sample.last={};QueryPerformanceCounter(&sample.activity);sample.dropped=false;}
unsigned next_settle(){auto& x=state().sample_rng;x^=x<<13;x^=x>>17;x^=x<<5;return 32+x%65;}
void reset_search(){auto& s=state();if(s.search){const auto old=s.search->status();s.past_accepted+=old.accepted;s.past_rejected+=old.rejected;s.past_unstable+=old.unstable;}std::vector<arc::WorkerMode> modes;if(s.supported){modes={arc::WorkerMode::Prefer,arc::WorkerMode::Core};if(s.allow_partition&&s.plan.partition_possible&&!s.conflict)modes.push_back(arc::WorkerMode::Partition);}s.search=std::make_unique<arc::WorkerPlacementSearch>(std::move(modes));}
std::vector<ULONG> worker_ids(){const auto& ids=state().plan.worker_sets;return {ids.begin(),ids.end()};}
bool exited(const Entry& e){DWORD code{};return e.process&&GetExitCodeProcess(e.handle,&code)&&code!=STILL_ACTIVE;}
void apply(Entry& e,Mode mode){
    if(exited(e))return;
    try{
        if(mode==Mode::Core||mode==Mode::Partition)set_sets(e.handle,e.process,worker_ids());else set_sets(e.handle,e.process,e.original);
        auto thread=e.process?e.primary:e.handle;
        if(mode==Mode::Prefer&&!thread)throw std::runtime_error("worker_primary_thread_unavailable");
        if(thread&&e.ideal_known){GROUP_AFFINITY affinity{};check(GetThreadGroupAffinity(thread,&affinity));auto ideal=e.ideal;
            if(mode!=Mode::Normal){ideal.Group=static_cast<WORD>(state().plan.group);ideal.Number=static_cast<BYTE>(state().plan.logical);ideal.Reserved=0;}
            const bool allowed=ideal.Group==affinity.Group&&ideal.Number<sizeof(affinity.Mask)*8&&(affinity.Mask&(KAFFINITY(1)<<ideal.Number));
            if(allowed)check(SetThreadIdealProcessorEx(thread,&ideal,nullptr));else if(mode!=Mode::Normal)throw std::runtime_error("worker_hard_affinity_conflict");
        }
    }catch(...){if(!exited(e))throw;}
}
bool restore_game(){auto& s=state();if(!s.partition_owned)return true;
    const auto current=sets(GetCurrentProcess(),true);
    if(current!=s.installed_game){s.partition_owned=false;s.conflict=true;s.error="game_cpu_sets_changed_externally";return false;}
    set_sets(GetCurrentProcess(),true,s.original_game);s.partition_owned=false;return true;
}
void initialize_locked(){auto& s=state();if(s.initialized)return;s.initialized=true;
    s.original_game=sets(GetCurrentProcess(),true);wchar_t allowed[4]{};s.allow_partition=GetEnvironmentVariableW(L"ARC_WORKER_ALLOW_PARTITION",allowed,4)==1&&allowed[0]==L'1';
    // Do not narrow a multi-group application's accessible groups accidentally.
    if(GetActiveProcessorGroupCount()!=1){s.error="multi_group_placement_not_qualified";return;}
    DWORD_PTR process_mask{},system_mask{};check(GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask));
    s.original_affinity=process_mask;
    ULONG bytes=0;GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    if(!bytes||bytes>1024*1024)throw std::runtime_error("CPU topology unavailable");std::vector<std::byte> buffer(bytes);
    check(GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),bytes,&bytes,GetCurrentProcess(),0));
    std::vector<arc::WorkerCpu> cpus;
    for(std::size_t offset=0;offset<bytes;){
        if(bytes-offset<sizeof(DWORD)*2)throw std::runtime_error("CPU topology truncated");const auto* info=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data()+offset);
        if(info->Size<sizeof(DWORD)*2||info->Size>bytes-offset)throw std::runtime_error("CPU topology size");
        if(info->Type==CpuSetInformation){if(info->Size<sizeof(SYSTEM_CPU_SET_INFORMATION))throw std::runtime_error("CPU set structure truncated");const auto& c=info->CpuSet;
            const bool allowed=c.LogicalProcessorIndex<sizeof(process_mask)*8&&(process_mask&(DWORD_PTR(1)<<c.LogicalProcessorIndex))&&(s.original_game.empty()||std::binary_search(s.original_game.begin(),s.original_game.end(),c.Id));
            cpus.push_back({c.Id,c.Group,c.LogicalProcessorIndex,c.CoreIndex,c.EfficiencyClass,allowed,c.Parked!=0,c.Allocated&&!c.AllocatedToTargetProcess});}
        offset+=info->Size;
    }
    s.plan=arc::plan_worker_placement(cpus);s.supported=!s.plan.worker_sets.empty();if(!s.supported)s.error="no_complete_available_worker_core";
}
bool change_locked(Mode mode){auto& s=state();initialize_locked();
    if(mode!=Mode::Normal&&s.supported){DWORD_PTR process_mask{},system_mask{};check(GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask));if(process_mask!=s.original_affinity){change_locked(Mode::Normal);s.error="process_hard_affinity_changed";return false;}}
    if(mode!=Mode::Normal&&!s.supported){s.error="placement_not_supported";return false;}
    if(mode==Mode::Partition&&(!s.allow_partition||!s.plan.partition_possible||s.conflict)){s.error="partition_not_permitted_or_insufficient_cores";return false;}
    if(mode==s.mode&&!(mode==Mode::Normal&&(s.partition_owned||s.rollback_pending)))return true;
    if(s.partition_owned&&!restore_game()&&mode==Mode::Partition)return false;
    try{
        if(mode==Mode::Partition){if(sets(GetCurrentProcess(),true)!=s.original_game)throw std::runtime_error("game_default_cpu_sets_changed");s.installed_game.assign(s.plan.game_sets.begin(),s.plan.game_sets.end());set_sets(GetCurrentProcess(),true,s.installed_game);s.partition_owned=true;}
        for(auto& e:s.entries)if(e.handle)apply(e,mode);
        s.mode=mode;s.published_mode=static_cast<unsigned>(mode);s.rollback_pending=false;++s.transitions;return true;
    }catch(const std::exception& error){++s.failures;s.error=error.what();
        s.rollback_pending=false;
        for(auto& e:s.entries)if(e.handle)try{apply(e,Mode::Normal);}catch(...){++s.failures;s.rollback_pending=true;}
        try{restore_game();}catch(...){++s.failures;s.rollback_pending=true;}s.mode=Mode::Normal;s.published_mode=0;return false;
    }
}
}
bool configure(const wchar_t* mode)noexcept{try{if(!mode)return false;std::lock_guard lock(state().mutex);
    if(wcscmp(mode,L"adaptive")==0){auto& s=state();if(!change_locked(Mode::Normal))return false;reset_search();s.epoch_known=false;LARGE_INTEGER seed{};QueryPerformanceCounter(&seed);s.sample_seed=static_cast<std::uint32_t>(seed.QuadPart)^GetCurrentProcessId();if(!s.sample_seed)s.sample_seed=1;s.sample_rng=s.sample_seed;auto& sample=samples();{std::lock_guard guard(sample.mutex);sample.swap=nullptr;LARGE_INTEGER f{};QueryPerformanceFrequency(&f);sample.frequency=double(f.QuadPart);}reset_samples(240);s.adapting=true;return true;}
    if(wcscmp(mode,L"normal")&&wcscmp(mode,L"prefer")&&wcscmp(mode,L"core")&&wcscmp(mode,L"partition"))return false;
    state().adapting=false;
    if(wcscmp(mode,L"normal")==0)return change_locked(Mode::Normal);if(wcscmp(mode,L"prefer")==0)return change_locked(Mode::Prefer);if(wcscmp(mode,L"core")==0)return change_locked(Mode::Core);if(wcscmp(mode,L"partition")==0)return change_locked(Mode::Partition);return false;
}catch(const std::exception& e){std::lock_guard lock(state().mutex);++state().failures;state().error=e.what();return false;}}
bool initialize()noexcept{wchar_t mode[32]{};const auto n=GetEnvironmentVariableW(L"ARC_WORKER_PLACEMENT",mode,32);if(n>=32)return false;return configure(n?mode:L"normal");}
Lease::Lease(HANDLE process,HANDLE primary_thread)noexcept{
    auto& s=state();std::lock_guard lock(s.mutex);Entry e;e.process=process!=nullptr;
    try{initialize_locked();const auto native=process?process:GetCurrentThread();check(DuplicateHandle(GetCurrentProcess(),native,GetCurrentProcess(),&e.handle,0,FALSE,DUPLICATE_SAME_ACCESS));
        if(primary_thread)check(DuplicateHandle(GetCurrentProcess(),primary_thread,GetCurrentProcess(),&e.primary,0,FALSE,DUPLICATE_SAME_ACCESS));
        e.original=sets(e.handle,e.process);if(e.process&&s.partition_owned&&e.original==s.installed_game)e.original=s.original_game;
        const auto thread=e.process?e.primary:e.handle;if(thread){check(GetThreadIdealProcessorEx(thread,&e.ideal));e.ideal_known=true;}
        for(unsigned i=0;i<s.entries.size();++i)if(!s.entries[i].handle){if(s.mode!=Mode::Normal)apply(e,s.mode);s.entries[i]=std::move(e);slot_=i;return;}throw std::runtime_error("Worker placement capacity");
    }catch(const std::exception& error){++s.failures;s.error=error.what();if(e.handle){try{apply(e,Mode::Normal);}catch(...){}CloseHandle(e.handle);}if(e.primary)CloseHandle(e.primary);}
}
Lease::~Lease(){if(slot_>=32)return;auto& s=state();std::lock_guard lock(s.mutex);auto& e=s.entries[slot_];try{if(s.mode!=Mode::Normal)apply(e,Mode::Normal);}catch(const std::exception& error){++s.failures;s.error=error.what();}CloseHandle(e.handle);if(e.primary)CloseHandle(e.primary);e={};}
void present(void* swap)noexcept{auto& s=state();s.presents[s.published_mode.load(std::memory_order_relaxed)].fetch_add(1,std::memory_order_relaxed);if(!s.adapting.load(std::memory_order_relaxed))return;
    auto& sample=samples();std::unique_lock lock(sample.mutex,std::try_to_lock);if(!lock.owns_lock()){sample.dropped=true;return;}
    if(sample.swap&&sample.swap!=swap)return;sample.swap=swap;LARGE_INTEGER now{};QueryPerformanceCounter(&now);const auto previous=sample.last;sample.last=sample.activity=now;
    if(sample.settle){--sample.settle;return;}if(!previous.QuadPart||sample.count==sample.frames.size())return;
    const double ms=double(now.QuadPart-previous.QuadPart)*1000/sample.frequency;if(ms<=0||ms>1000){sample.dropped=true;return;}sample.frames[sample.count++]=ms;
}
bool adaptive()noexcept{return state().adapting.load(std::memory_order_relaxed);}
void collect(std::uint64_t epoch,std::uint64_t pipeline)noexcept{if(!adaptive())return;auto& s=state();try{std::lock_guard lock(s.mutex);if(!s.adapting||!s.search)return;
    if(s.failures){s.adapting=false;change_locked(Mode::Normal);return;}
    if(!s.epoch_known){s.epoch_known=true;s.workload_epoch=epoch;s.workload_pipeline=pipeline;}
    else if(s.workload_epoch!=epoch||s.workload_pipeline!=pipeline){s.workload_epoch=epoch;s.workload_pipeline=pipeline;++s.resets;if(!change_locked(Mode::Normal)){s.adapting=false;return;}reset_search();reset_samples(64);return;}
    std::array<double,120> values;bool dropped=false,idle=false,harmful=false;
    {auto& sample=samples();std::lock_guard guard(sample.mutex);LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        idle=sample.activity.QuadPart&&double(now.QuadPart-sample.activity.QuadPart)>sample.frequency*2;
        if(idle){sample.swap=nullptr;sample.last={};}
        else if(sample.count<sample.frames.size()){
            harmful=(s.mode!=Mode::Normal&&sample.dropped.load())||(sample.count>=16&&s.search->harmful(std::accumulate(sample.frames.begin(),sample.frames.begin()+sample.count,0.0)/sample.count,sample.count));
            if(!harmful)return;
        }else{values=sample.frames;sample.count=0;dropped=sample.dropped.exchange(false);}}
    if(idle||harmful){if(harmful)s.search->abort();else reset_search();if(!change_locked(Mode::Normal))s.adapting=false;reset_samples(64);return;}
    std::sort(values.begin(),values.end());arc::PlacementWindow window;window.frames=static_cast<unsigned>(values.size());window.valid=!dropped;
    window.mean=std::accumulate(values.begin(),values.end(),0.0)/values.size();window.p95=values[113];window.p99=values[118];s.last_mean=window.mean;s.last_p95=window.p95;s.last_p99=window.p99;++s.windows;
    const auto wanted=static_cast<Mode>(s.search->observe(window));const bool changed=wanted!=s.mode;
    if(!change_locked(wanted)){s.adapting=false;change_locked(Mode::Normal);return;}if(changed)reset_samples(next_settle());
}catch(const std::exception& error){std::lock_guard lock(s.mutex);++s.failures;s.error=error.what();s.adapting=false;try{change_locked(Mode::Normal);}catch(...) {}}}
void snapshot(std::ostream& out){auto& s=state();std::lock_guard lock(s.mutex);unsigned workers=0;for(const auto& e:s.entries)workers+=e.handle!=nullptr;
    out<<"{\"mode\":"<<std::quoted(name(s.mode))<<",\"supported\":"<<(s.supported?"true":"false")<<",\"worker_group\":"<<s.plan.group<<",\"worker_core\":"<<s.plan.core<<",\"physical_cores\":"<<s.plan.physical_cores<<",\"worker_cpu_sets\":[";bool first=true;for(auto id:s.plan.worker_sets){if(!first)out<<',';first=false;out<<id;}
    out<<"],\"worker_logicals\":[";first=true;for(auto id:s.plan.worker_logicals){if(!first)out<<',';first=false;out<<id;}
    out<<"],\"registered_workers\":"<<workers<<",\"game_default_partitioned\":"<<(s.partition_owned?"true":"false")<<",\"rollback_pending\":"<<(s.rollback_pending?"true":"false")<<",\"os_exclusive_reservation\":false,\"transitions\":"<<s.transitions<<",\"presents_by_mode\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<s.presents[i].load();}out<<"],\"failures\":"<<s.failures<<",\"last_error\":"<<std::quoted(s.error)<<",\"adaptive_enabled\":"<<(s.adapting?"true":"false")<<",\"adaptive_windows\":"<<s.windows<<",\"workload_resets\":"<<s.resets<<",\"sample_seed\":"<<s.sample_seed<<",\"last_window_ms\":["<<s.last_mean<<','<<s.last_p95<<','<<s.last_p99<<']';
    if(s.search){const auto search=s.search->status();out<<",\"search\":{\"accepted\":"<<search.accepted<<",\"rejected\":"<<search.rejected<<",\"unstable\":"<<search.unstable<<",\"accepted_total\":"<<s.past_accepted+search.accepted<<",\"rejected_total\":"<<s.past_rejected+search.rejected<<",\"unstable_total\":"<<s.past_unstable+search.unstable<<",\"requested\":"<<std::quoted(name(search.requested))<<",\"done\":"<<(search.done?"true":"false")<<",\"reason\":"<<std::quoted(search.reason)<<'}';}out<<'}';
}
}
