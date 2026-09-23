#include "decoder.hpp"
#include "capture.hpp"
#include "cost_clock.hpp"
#include "arc/cpu/runtime_cost.hpp"
#include "arc/cpu/publication.hpp"
#include "drmgr.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstring>
#include <new>
#include <utility>
#include <xmmintrin.h>

using namespace arc::cpu;
using namespace arc::cpu::dbi;
namespace {
enum class Mode { Study, Neutral, Apply };
enum class Actuator { Auto, Specialize, Memo, Incremental };
constexpr unsigned max_regions = 64, max_observations = 128, hot_threshold = 32;
struct Candidate {
    Decoded decoded{};
    Analysis analysis{};
    std::atomic<std::uint64_t> calls{0}, executions{0}, guards{0}, misses{0},
        reused_nodes{0}, dirty_nodes{0};
    std::atomic<bool> active{false};
    std::uint64_t generation{};
    std::uint64_t discovery_ticks{};
    std::uint64_t last_seen{}, recent_hits{};
    app_pc module_start{}, module_end{};
    arc::PublishedCandidate publication{};
    arc::CandidateFingerprint fingerprint{};
    std::atomic<unsigned> sample_status{0};
    State sample{};
    RuntimeCost::Decision cost_decision{};
    std::array<unsigned,4> cost_sample_counts{}, cost_actuations{};
    std::array<double,4> cost_medians{}, cost_spreads{};
    std::uint64_t tracking_ticks{};
    unsigned cost_snapshot_tid{};
    std::uint64_t timed_spans{}, timing_drops{}, auto_trial_executions{}, auto_policy_executions{};
};
struct LocalCache {
    unsigned candidate{~0u};
    std::uint64_t generation{};
    alignas(RegionCache) byte storage[sizeof(RegionCache)]{};
    RegionCache* cache{};
    Specialization spec{};
    ~LocalCache() { if (cache) cache->~RegionCache(); }
    RegionCache& get(unsigned id, std::uint64_t gen, const Analysis& a) {
        if (candidate != id || generation != gen) {
            if (cache) cache->~RegionCache();
            cache = new (storage) RegionCache(a);
            spec = {};
            candidate = id;
            generation = gen;
        }
        return *cache;
    }
};
struct PendingCost {
    bool active{}, actuated{};
    unsigned id{}, local_slot{};
    std::uint64_t generation{}, started{};
    app_pc end{};
    CostAction action{CostAction::Original};
};
// Cost histories survive data-cache eviction: one fixed policy per candidate slot.
struct ThreadState { LocalCache slot[4]; RuntimeCost policy[max_regions]; PendingCost pending{}; };
struct BuildContext { std::uintptr_t token{}; app_pc end{}; };
struct Observation {
    Decoded decoded{};
    std::uint64_t serial{}, calls{}, last_seen{}, recent_hits{};
    std::uint64_t decode_ticks{};
    unsigned candidate{~0u};
    std::uint64_t candidate_generation{};
};
Candidate candidates[max_regions];
Observation observations[max_observations];
std::atomic<unsigned> candidate_count{};
std::uint64_t next_generation{1}, next_observation_serial{1}, observation_clock{};
std::atomic<std::uint64_t> replacements{0}, observations_seen{0};
std::atomic<std::uint64_t> total_calls{0},total_executions{0},total_guards{0},total_misses{0};
void* candidate_lock{};
int tls_slot{-1};
Mode mode{Mode::Neutral};
Actuator actuator{Actuator::Auto};
char output_path[MAXIMUM_PATH]{}, stop_path[MAXIMUM_PATH]{};
std::atomic<bool> stopped{false}, ending{false}, conflict{false};
std::atomic<std::uint64_t> rejected{0}, discovered{0}, code_changed{0};
std::atomic<std::uint64_t> protection_events{0}, module_unloads{0};
std::atomic<std::uint64_t> range_invalidations{0};
app_pc main_start{}, main_end{};
int protect_sysnum{-1};
// DynamoRIO's private loader does not run this client's dynamic C++
// initializers. Construct the configured budget explicitly before callbacks.
alignas(DiscoveryCapture) byte capture_storage[sizeof(DiscoveryCapture)]{};
DiscoveryCapture* capture{};
CostClock cost_clock{};
std::uint64_t start_us{};
file_t stop_file{INVALID_FILE};
void* stop_map{};
size_t stop_map_size{sizeof(std::uint32_t)};
bool stop_control_available{};

#include "client_cost.inl"
void parse_args(int argc, const char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "-mode")) {
            ++i;
            if (!strcmp(argv[i], "study")) mode = Mode::Study;
            else if (!strcmp(argv[i], "apply")) mode = Mode::Apply;
            else mode = Mode::Neutral;
        } else if (!strcmp(argv[i], "-actuator")) {
            ++i;
            if (!strcmp(argv[i], "specialize")) actuator = Actuator::Specialize;
            else if (!strcmp(argv[i], "memo")) actuator = Actuator::Memo;
            else if (!strcmp(argv[i], "incremental")) actuator = Actuator::Incremental;
            else actuator = Actuator::Auto;
        } else if (!strcmp(argv[i], "-out")) {
            dr_snprintf(output_path, sizeof(output_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "-stop")) {
            dr_snprintf(stop_path, sizeof(stop_path), "%s", argv[++i]);
        }
    }
}
void check_stop_control() {
    if (!stop_map || stopped.load(std::memory_order_acquire)) return;
    auto& word=*static_cast<std::uint32_t*>(stop_map);
    if (!std::atomic_ref<std::uint32_t>(word).load(std::memory_order_acquire)) return;
    if (stopped.exchange(true,std::memory_order_acq_rel)) return;
    capture->cancel();
    dr_mutex_lock(candidate_lock);
    for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i)
        candidates[i].publication.invalidate(arc::CandidateReason::UserStopped);
    dr_mutex_unlock(candidate_lock);
}
bool read_state(const dr_mcontext_t& mc, State& s) {
    const std::uint64_t r[] = {mc.xax,mc.xcx,mc.xdx,mc.xbx,mc.xsp,mc.xbp,mc.xsi,mc.xdi,
                               mc.r8,mc.r9,mc.r10,mc.r11,mc.r12,mc.r13,mc.r14,mc.r15};
    for (unsigned i=0;i<register_count;++i) s.regs[i]=r[i];
    s.rflags=mc.xflags;
    return true;
}
void write_state(dr_mcontext_t& mc, const RunResult& result) {
    auto& r=result.state.regs;
    const auto m=result.output_mask;
#define SET_REG(i,field) if (m & (1u << i)) mc.field = r[i]
    SET_REG(0,xax); SET_REG(1,xcx); SET_REG(2,xdx); SET_REG(3,xbx);
    SET_REG(5,xbp); SET_REG(6,xsi); SET_REG(7,xdi); SET_REG(8,r8);
    SET_REG(9,r9); SET_REG(10,r10); SET_REG(11,r11); SET_REG(12,r12);
    SET_REG(13,r13); SET_REG(14,r14); SET_REG(15,r15);
#undef SET_REG
}
bool code_matches(const Candidate& c) {
    dr_mem_info_t info{};
    if (!dr_query_memory_ex(c.decoded.start,&info) ||
        (info.prot & (DR_MEMPROT_EXEC|DR_MEMPROT_WRITE)) != DR_MEMPROT_EXEC ||
        c.decoded.end > info.base_pc+info.size) return false;
    for (std::size_t i=0;i<c.decoded.byte_count;++i) {
        byte v{};
        if (!dr_safe_read(c.decoded.start+i,1,&v,nullptr) || v!=c.decoded.original_bytes[i]) return false;
    }
    return true;
}
int syscall_number(const char* name) {
    auto* module=dr_lookup_module_by_name("ntdll.dll");
    if (!module) return -1;
    const auto* pc=reinterpret_cast<const byte*>(dr_get_proc_address(module->handle,name));
    byte bytes[8]{};
    const bool read=pc && dr_safe_read(pc,sizeof(bytes),bytes,nullptr);
    dr_free_module_data(module);
    if (!read || bytes[0]!=0x4c || bytes[1]!=0x8b || bytes[2]!=0xd1 || bytes[3]!=0xb8) return -1;
    int n{}; std::memcpy(&n,bytes+4,sizeof(n)); return n;
}
bool overlaps(app_pc begin, app_pc end, std::uintptr_t address, std::size_t size) {
    if (!size) return false;
    const auto lo=reinterpret_cast<std::uintptr_t>(begin);
    const auto hi=reinterpret_cast<std::uintptr_t>(end);
    const auto page=dr_page_size();
    const auto first=address & ~(static_cast<std::uintptr_t>(page)-1);
    const auto requested_end=address > UINTPTR_MAX-size ? UINTPTR_MAX : address+size;
    const auto last=requested_end > UINTPTR_MAX-(page-1) ? UINTPTR_MAX :
        (requested_end+page-1)&~(static_cast<std::uintptr_t>(page)-1);
    return first<hi && lo<last;
}
bool pre_syscall(void* dc,int number) {
    if (number==protect_sysnum) {
        ++protection_events;
        const auto process=static_cast<std::uintptr_t>(dr_syscall_get_param(dc,0));
        // The pseudo handle is the common VirtualProtect path. Real handles
        // are checked so protection of another process cannot retire us.
        if (process!=UINTPTR_MAX && GetProcessId(reinterpret_cast<HANDLE>(process))!=dr_get_process_id())
            return true;
        const auto base_ptr=reinterpret_cast<const void*>(dr_syscall_get_param(dc,1));
        const auto size_ptr=reinterpret_cast<const void*>(dr_syscall_get_param(dc,2));
        std::uintptr_t base{}; std::size_t size{};
        const bool known=dr_safe_read(base_ptr,sizeof(base),&base,nullptr) &&
                         dr_safe_read(size_ptr,sizeof(size),&size,nullptr);
        dr_mutex_lock(candidate_lock);
        for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i) {
            auto& c=candidates[i];
            if (!known || overlaps(c.decoded.start,c.decoded.end,base,size)) {
                c.active.store(false,std::memory_order_release);
                c.publication.invalidate(arc::CandidateReason::GenerationChanged);
                ++range_invalidations;
            }
        }
        for (auto& o:observations)
            if (o.serial && (!known || overlaps(o.decoded.start,o.decoded.end,base,size))) {
                o.serial=0; o.candidate=~0u;
            }
        dr_mutex_unlock(candidate_lock);
    }
    return true;
}
bool filter_syscall(void*,int number) { return number==protect_sysnum; }
#include "client_execution.inl"

void thread_init(void* dc) {
    auto* p=static_cast<ThreadState*>(dr_thread_alloc(dc,sizeof(ThreadState)));
    new (p) ThreadState();
    drmgr_set_tls_field(dc,tls_slot,p);
}
void thread_exit(void* dc) {
    auto* p=static_cast<ThreadState*>(drmgr_get_tls_field(dc,tls_slot));
    if (p) { p->~ThreadState(); dr_thread_free(dc,p,sizeof(ThreadState)); }
}
void module_unload(void*,const module_data_t* m) {
    bool found=false;
    dr_mutex_lock(candidate_lock);
    for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i)
        if (candidates[i].module_start==m->start) {
            found=true;
            capture->forget_novelty(candidates[i].generation);
            candidates[i].active.store(false,std::memory_order_release);
            candidates[i].publication.invalidate(arc::CandidateReason::GenerationChanged);
        }
    for (auto& o:observations)
        if (o.serial && o.decoded.start>=m->start && o.decoded.start<m->end)
            o.serial=0;
    dr_mutex_unlock(candidate_lock);
    if (found) ++module_unloads;
}
void module_load(void*,const module_data_t* m,bool) {
    const char* name=dr_module_preferred_name(m);
    if (name && (strstr(name,"arc-dx12-probe") || strstr(name,"arc_dx12_probe")))
        conflict.store(true,std::memory_order_release);
}
#include "client_registry.inl"

dr_emit_flags_t analyze_block(void*,void*,instrlist_t* bb,bool for_trace,bool translating,void** user_data) {
    *user_data=nullptr;
    if (translating || stopped.load(std::memory_order_relaxed) ||
        conflict.load(std::memory_order_relaxed)) return DR_EMIT_DEFAULT;
    auto* first=instrlist_first_app(bb);
    if (!first || !instr_get_app_pc(first) || instr_get_app_pc(first)<main_start ||
        instr_get_app_pc(first)>=main_end) return DR_EMIT_DEFAULT;
    const auto decode_started=cost_clock.now();
    Decoded d{};
    if (!decode_block(bb,d)) { ++rejected; return DR_EMIT_DEFAULT; }
    const auto decode_finished=cost_clock.now();
    const auto decode_ticks=decode_finished>decode_started ? decode_finished-decode_started : 0;
    if (d.start<main_start || d.start>=main_end || d.end>main_end) return DR_EMIT_DEFAULT;
    dr_mutex_lock(candidate_lock);
    unsigned id=candidate_count.load(std::memory_order_relaxed);
    for (unsigned i=0;i<id;++i)
        if (same_code(candidates[i].decoded,d)) { id=i; break; }
    if (id==candidate_count.load(std::memory_order_relaxed))
        for (unsigned i=0;i<id;++i)
            if (candidates[i].decoded.start==d.start &&
                candidates[i].publication.outstanding()==0 &&
                (!candidates[i].active.load(std::memory_order_acquire) || !code_matches(candidates[i]))) {
                candidates[i].active.store(false,std::memory_order_release);
                candidates[i].publication.invalidate(arc::CandidateReason::GenerationChanged);
                id=i; break;
            }
    if (id<max_regions && id==candidate_count.load(std::memory_order_relaxed)) {
        admit_slot(id,d,0,decode_ticks);
        candidate_count.store(id+1,std::memory_order_release);
    } else if (id<max_regions && !candidates[id].active.load(std::memory_order_acquire) &&
               candidates[id].publication.outstanding()==0 &&
               !stopped.load(std::memory_order_acquire)) {
        admit_slot(id,d,0,decode_ticks);
    }
    if (id<max_regions) {
        const auto gen=candidates[id].generation;
        *user_data=reinterpret_cast<void*>(static_cast<std::uintptr_t>((gen<<8)|(id+1)));
    } else {
        unsigned observation=max_observations;
        std::uint64_t oldest=UINT64_MAX;
        for (unsigned i=0;i<max_observations;++i) {
            if (observations[i].serial && same_code(observations[i].decoded,d)) {
                observation=i; break;
            }
            if (!observations[i].serial) { observation=i; break; }
            if (observations[i].last_seen<oldest) { oldest=observations[i].last_seen; observation=i; }
        }
        auto& o=observations[observation];
        if (!o.serial || !same_code(o.decoded,d)) {
            o.decoded=d; o.decode_ticks=decode_ticks; o.serial=next_observation_serial++;
            o.calls=0; o.recent_hits=0; o.candidate=~0u; o.candidate_generation=0;
        }
        o.last_seen=++observation_clock;
        *user_data=reinterpret_cast<void*>(static_cast<std::uintptr_t>((o.serial<<8)|(observation+max_regions+1)));
    }
    const auto token=reinterpret_cast<std::uintptr_t>(*user_data);
    dr_mutex_unlock(candidate_lock);
    auto* context=static_cast<BuildContext*>(dr_thread_alloc(dr_get_current_drcontext(),sizeof(BuildContext)));
    context->token=token; context->end=d.end; *user_data=context;
    return DR_EMIT_MUST_END_TRACE;
}
dr_emit_flags_t insert_block(void* dc,void*,instrlist_t* bb,instr_t* ins,bool,bool,void* user_data) {
    // drmgr also visits trailing metadata after the last app instruction,
    // where the per-BB context has already been released.
    if(!instr_is_app(ins)) return DR_EMIT_DEFAULT;
    auto* context=static_cast<BuildContext*>(user_data);
    const auto pc=instr_get_app_pc(ins);
    bool boundary=context && pc==context->end;
    if(mode==Mode::Apply && actuator==Actuator::Auto && pc && ins==instrlist_first_app(bb)) {
        // Translation-time scan only. Do not add a timing callback to every
        // game basic block when only a bounded set of exits is being studied.
        dr_mutex_lock(candidate_lock);
        for(unsigned i=0;i<candidate_count.load();++i)
            if(candidates[i].decoded.end==pc) { boundary=true; break; }
        dr_mutex_unlock(candidate_lock);
    }
    if(mode==Mode::Apply && actuator==Actuator::Auto && pc && boundary)
        dr_insert_clean_call(dc,bb,ins,reinterpret_cast<void*>(cost_boundary),false,1,OPND_CREATE_INTPTR(pc));
    if (context && ins==instrlist_first_app(bb)) {
        const auto token=context->token;
        const unsigned encoded=static_cast<unsigned>((token&255u)-1u);
        const auto generation=static_cast<std::uint64_t>(token>>8);
        if (encoded<max_regions)
            dr_insert_clean_call(dc,bb,ins,reinterpret_cast<void*>(on_region),false,3,
                OPND_CREATE_INT32(encoded),OPND_CREATE_INTPTR(generation),OPND_CREATE_INTPTR(pc));
        else
            dr_insert_clean_call(dc,bb,ins,reinterpret_cast<void*>(on_observed),false,3,
                OPND_CREATE_INT32(encoded-max_regions),OPND_CREATE_INTPTR(generation),
                OPND_CREATE_INTPTR(instr_get_app_pc(ins)));
    }
    if(context && ins==instrlist_last_app(bb)) dr_thread_free(dc,context,sizeof(BuildContext));
    return DR_EMIT_DEFAULT;
}
#include "client_report.inl"

}
DR_EXPORT void dr_client_main(client_id_t,int argc,const char* argv[]) {
    dr_set_client_name("ARC CPU backend","https://github.com/Flyingzaptop/ARC");
    parse_args(argc,argv);
    cost_clock.initialize();
    capture=new (capture_storage) DiscoveryCapture();
    start_us=dr_get_microseconds();
    if (!drmgr_init()) return;
    tls_slot=drmgr_register_tls_field();
    candidate_lock=dr_mutex_create();
    protect_sysnum=syscall_number("NtProtectVirtualMemory");
    if (protect_sysnum>=0) {
        dr_register_filter_syscall_event(filter_syscall);
        drmgr_register_pre_syscall_event(pre_syscall);
    }
    if (auto* m=dr_get_main_module()) { main_start=m->start; main_end=m->end; dr_free_module_data(m); }
    drmgr_register_thread_init_event(thread_init);
    drmgr_register_thread_exit_event(thread_exit);
    drmgr_register_module_load_event(module_load);
    drmgr_register_module_unload_event(module_unload);
    drmgr_register_bb_instrumentation_event(analyze_block,insert_block,nullptr);
    dr_register_exit_event(exit_event);
    if (stop_path[0]) {
        stop_file=dr_open_file(stop_path,DR_FILE_READ);
        if (stop_file!=INVALID_FILE) {
            stop_map=dr_map_file(stop_file,&stop_map_size,0,nullptr,DR_MEMPROT_READ,0);
            // Keep the mapping, release DR's read handle so the controller can
            // open the same file for a shared DWORD write on Windows.
            dr_close_file(stop_file);
            stop_file=INVALID_FILE;
        }
        stop_control_available=stop_map && stop_map_size>=sizeof(std::uint32_t);
        if (!stop_control_available) stopped.store(true,std::memory_order_release);
    }
}
