#include "decoder.hpp"
#include "capture.hpp"
#include "arc/cpu/publication.hpp"
#include "drmgr.h"
#include <atomic>
#include <cstring>
#include <new>
#include <utility>

using namespace arc::cpu;
using namespace arc::cpu::dbi;
namespace {
enum class Mode { Study, Neutral, Apply };
enum class Actuator { Auto, Specialize, Memo, Incremental };
constexpr unsigned max_regions = 64, hot_threshold = 32;
struct Candidate {
    Decoded decoded{};
    Analysis analysis{};
    std::atomic<std::uint64_t> calls{0}, executions{0}, guards{0}, misses{0},
        reused_nodes{0}, dirty_nodes{0};
    std::atomic<bool> active{false};
    std::uint64_t generation{};
    app_pc module_start{}, module_end{};
    arc::PublishedCandidate publication{};
    arc::CandidateFingerprint fingerprint{};
    std::atomic<unsigned> sample_status{0};
    State sample{};
};
struct LocalCache {
    unsigned candidate{~0u};
    alignas(RegionCache) byte storage[sizeof(RegionCache)]{};
    RegionCache* cache{};
    Specialization spec{};
    ~LocalCache() { if (cache) cache->~RegionCache(); }
    RegionCache& get(unsigned id, const Analysis& a) {
        if (candidate != id) {
            if (cache) cache->~RegionCache();
            cache = new (storage) RegionCache(a);
            spec = {};
            candidate = id;
        }
        return *cache;
    }
};
struct ThreadState { LocalCache slot[4]; };
Candidate candidates[max_regions];
std::atomic<unsigned> candidate_count{};
void* candidate_lock{};
int tls_slot{-1};
Mode mode{Mode::Neutral};
Actuator actuator{Actuator::Auto};
char output_path[MAXIMUM_PATH]{}, stop_path[MAXIMUM_PATH]{};
std::atomic<bool> stopped{false}, ending{false}, conflict{false};
std::atomic<std::uint64_t> rejected{0}, discovered{0}, code_changed{0};
std::atomic<std::uint64_t> protection_events{0}, module_unloads{0};
app_pc main_start{}, main_end{};
int protect_sysnum{-1};
DiscoveryCapture capture{};
std::uint64_t start_us{};
file_t stop_file{INVALID_FILE};
void* stop_map{};
size_t stop_map_size{sizeof(std::uint32_t)};
bool stop_control_available{};

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
    capture.cancel();
    for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i)
        candidates[i].publication.invalidate(arc::CandidateReason::UserStopped);
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
bool pre_syscall(void*,int number) {
    if (number==protect_sysnum) {
        ++protection_events;
        for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i) {
            candidates[i].active.store(false,std::memory_order_release);
            candidates[i].publication.invalidate(arc::CandidateReason::GenerationChanged);
        }
    }
    return true;
}
bool filter_syscall(void*,int number) { return number==protect_sysnum; }
void on_region(unsigned id) {
    struct FpScope {
        alignas(16) byte state[DR_FPSTATE_BUF_SIZE]{};
        FpScope() { proc_save_fpstate(state); }
        ~FpScope() { proc_restore_fpstate(state); }
        void before_redirect() { proc_restore_fpstate(state); }
    } fp;
    check_stop_control();
    Candidate& c=candidates[id];
    const auto calls=c.calls.fetch_add(1,std::memory_order_relaxed)+1;
    if (mode==Mode::Study && calls>=hot_threshold && c.sample_status.load(std::memory_order_acquire)==0) {
        unsigned expected=0;
        if (c.sample_status.compare_exchange_strong(expected,1)) {
            const auto observation=capture.try_observe(reinterpret_cast<std::uintptr_t>(c.decoded.start),
                c.decoded.region.count,c.decoded.byte_count+sizeof(State),c.analysis.live_in_mask,c.decoded.code_hash);
            if (observation!=DiscoveryCapture::Observation::Started &&
                observation!=DiscoveryCapture::Observation::Recorded) {
                c.sample_status.store(0,std::memory_order_release);
                return;
            }
            dr_mcontext_t mc{sizeof(mc),DR_MC_ALL};
            if (dr_get_mcontext(dr_get_current_drcontext(),&mc)) {
                read_state(mc,c.sample);
                c.sample_status.store(capture.finish(true)?2:0,std::memory_order_release);
            } else {
                capture.finish(false);
                c.sample_status.store(0,std::memory_order_release);
            }
        }
    }
    // Without an end-to-end cost estimate the automatic policy cannot claim
    // this tiny region repays clean-call, comparison and redirection costs.
    if (actuator==Actuator::Auto) return;
    if (mode!=Mode::Apply || calls<hot_threshold || stopped.load(std::memory_order_acquire) ||
        conflict.load(std::memory_order_acquire) || !c.active.load(std::memory_order_acquire)) return;
    if (!code_matches(c)) { c.active.store(false,std::memory_order_release);
        c.publication.invalidate(arc::CandidateReason::GenerationChanged); ++code_changed; return; }
    void* dc=dr_get_current_drcontext();
    auto* thread=static_cast<ThreadState*>(drmgr_get_tls_field(dc,tls_slot));
    if (!thread) return;
    auto lease=c.publication.try_acquire(c.fingerprint,
        guard_code_identity|guard_module_lifetime|guard_complete_inputs);
    if (!lease) return;
    dr_mcontext_t mc{sizeof(mc),DR_MC_ALL};
    if (!dr_get_mcontext(dc,&mc)) return;
    State state{}; read_state(mc,state);
    RunResult result{};
    if (actuator==Actuator::Specialize) {
        // A specialization is published from the first value observation in
        // this thread. The guard checks all required live-ins on every use.
        auto& slot=thread->slot[id%4];
        auto& cache=slot.get(id,c.analysis);
        if (!slot.spec.valid) {
            slot.spec=specialize(c.analysis,state,c.analysis.live_in_mask,c.generation);
            ++c.misses;
            return; // original instructions establish the first observation
        }
        bool guard=false;
        result=execute_specialized(c.analysis,slot.spec,state,c.generation,guard);
        if (guard) ++c.guards; else { ++c.misses; return; }
        (void)cache;
    } else {
        auto& cache=thread->slot[id%4].get(id,c.analysis);
        RegionCache::Action action=(actuator==Actuator::Incremental) ? RegionCache::Action::Incremental : RegionCache::Action::Memo;
        result=cache.run(action,state,c.generation);
        if (result.reused_nodes) ++c.guards; else { ++c.misses; return; }
    }
    if (!result.output_mask) return;
    c.reused_nodes.fetch_add(result.reused_nodes,std::memory_order_relaxed);
    c.dirty_nodes.fetch_add(result.evaluated_nodes,std::memory_order_relaxed);
    write_state(mc,result);
    mc.pc=c.decoded.end;
    ++c.executions;
    // Candidate/model storage is fixed for process lifetime. The immutable
    // projection has been consumed; release before non-returning redirect.
    lease.reset();
    fp.before_redirect();
    // Redirection is the execution boundary: on admission miss we return to
    // untouched original instructions; on hit the entire closed prefix is skipped.
    dr_redirect_execution(&mc);
}
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
    for (unsigned i=0;i<candidate_count.load(std::memory_order_acquire);++i)
        if (candidates[i].module_start==m->start) {
            found=true;
            candidates[i].active.store(false,std::memory_order_release);
            candidates[i].publication.invalidate(arc::CandidateReason::GenerationChanged);
        }
    if (found) ++module_unloads;
}
void module_load(void*,const module_data_t* m,bool) {
    const char* name=dr_module_preferred_name(m);
    if (name && (strstr(name,"arc-dx12-probe") || strstr(name,"arc_dx12_probe")))
        conflict.store(true,std::memory_order_release);
}
dr_emit_flags_t analyze_block(void*,void*,instrlist_t* bb,bool for_trace,bool translating,void** user_data) {
    *user_data=nullptr;
    if (translating || stopped.load(std::memory_order_relaxed) ||
        conflict.load(std::memory_order_relaxed)) return DR_EMIT_DEFAULT;
    auto* first=instrlist_first_app(bb);
    if (!first || !instr_get_app_pc(first) || instr_get_app_pc(first)<main_start ||
        instr_get_app_pc(first)>=main_end) return DR_EMIT_DEFAULT;
    Decoded d{};
    if (!decode_block(bb,d)) { ++rejected; return DR_EMIT_DEFAULT; }
    if (d.start<main_start || d.start>=main_end || d.end>main_end) return DR_EMIT_DEFAULT;
    dr_mutex_lock(candidate_lock);
    unsigned id=candidate_count.load(std::memory_order_relaxed);
    for (unsigned i=0;i<id;++i)
        if (candidates[i].decoded.start==d.start && candidates[i].decoded.byte_count==d.byte_count &&
            candidates[i].decoded.code_hash==d.code_hash &&
            std::memcmp(candidates[i].decoded.original_bytes.data(),d.original_bytes.data(),d.byte_count)==0) { id=i; break; }
    if (id==candidate_count.load(std::memory_order_relaxed) && id<max_regions) {
        auto& c=candidates[id]; c.decoded=d; c.analysis=analyze(d.region);
        c.module_start=main_start; c.module_end=main_end; c.generation=1;
        c.fingerprint.content=d.code_hash ? d.code_hash : 1;
        c.fingerprint.code_generation=c.generation;
        c.fingerprint.context=reinterpret_cast<std::uintptr_t>(main_start);
        CpuPublicationInput publication{};
        publication.id=id+1; publication.code_begin=reinterpret_cast<std::uintptr_t>(d.start);
        publication.code_end=reinterpret_cast<std::uintptr_t>(d.end);
        publication.original_entry=publication.code_begin;
        publication.variant_entry=reinterpret_cast<std::uintptr_t>(on_region);
        publication.fingerprint=c.fingerprint;
        publication.kind=actuator==Actuator::Incremental ? arc::CandidateKind::Incremental :
            actuator==Actuator::Memo ? arc::CandidateKind::ExactReuse : arc::CandidateKind::Specialization;
        dr_mem_info_t info{};
        const bool immutable_page=dr_query_memory_ex(d.start,&info) &&
            (info.prot & (DR_MEMPROT_EXEC|DR_MEMPROT_WRITE))==DR_MEMPROT_EXEC &&
            d.end<=info.base_pc+info.size;
        const auto admission=make_cpu_pod_admission(c.analysis,publication);
        c.active.store(immutable_page && protect_sysnum>=0 &&
            publish_cpu_candidate(c.publication,admission,true),std::memory_order_release);
        candidate_count.store(id+1,std::memory_order_release); ++discovered;
        if (mode==Mode::Study) capture.note_novelty();
    }
    dr_mutex_unlock(candidate_lock);
    if (id>=max_regions) return DR_EMIT_DEFAULT;
    *user_data=reinterpret_cast<void*>(static_cast<std::uintptr_t>(id+1));
    return DR_EMIT_MUST_END_TRACE;
}
dr_emit_flags_t insert_block(void* dc,void*,instrlist_t* bb,instr_t* ins,bool,bool,void* user_data) {
    if (user_data && ins==instrlist_first_app(bb)) {
        const auto id=static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(user_data)-1);
        dr_insert_clean_call(dc,bb,ins,reinterpret_cast<void*>(on_region),false,1,OPND_CREATE_INT32(id));
    }
    return DR_EMIT_DEFAULT;
}
void exit_event() {
    ending.store(true,std::memory_order_release);
    if (stop_map) dr_unmap_file(stop_map,stop_map_size);
    if (stop_file!=INVALID_FILE) dr_close_file(stop_file);
    if (!output_path[0]) return;
    const file_t f=dr_open_file(output_path,DR_FILE_WRITE_OVERWRITE);
    if (f==INVALID_FILE) return;
    std::uint64_t calls=0,exec=0,guards=0,misses=0;
    for (unsigned i=0;i<candidate_count.load();++i) {
        calls+=candidates[i].calls.load(); exec+=candidates[i].executions.load();
        guards+=candidates[i].guards.load(); misses+=candidates[i].misses.load();
    }
    const auto cap=capture.snapshot();
    dr_fprintf(f,"{\"backend\":\"dynamorio-11.3.0\",\"mode\":\"%s\",\"actuator\":\"%s\",\"pid\":%u,\"start_utc_us_since_1601\":%llu,\"regions_discovered\":%u,\"regions_rejected\":%llu,\"calls\":%llu,\"executions\":%llu,\"guard_hits\":%llu,\"guard_misses\":%llu,\"code_changed\":%llu,\"protection_events\":%llu,\"module_unloads\":%llu,\"stopped\":%s,\"stop_control_available\":%s,\"dx12_conflict\":%s,\"capture_started\":%llu,\"capture_completed\":%llu,\"capture_incomplete\":%llu,\"capture_events\":%llu,\"capture_bytes\":%llu,\"capture_rejected\":%llu,\"auto_application_disabled\":%s,\"regions\":[",
        mode==Mode::Apply?"apply":mode==Mode::Study?"study":"neutral",
        actuator==Actuator::Memo?"memo":actuator==Actuator::Incremental?"incremental":actuator==Actuator::Specialize?"specialize":"auto",
        static_cast<unsigned>(dr_get_process_id()),(unsigned long long)start_us,
        candidate_count.load(),(unsigned long long)rejected.load(),(unsigned long long)calls,(unsigned long long)exec,
        (unsigned long long)guards,(unsigned long long)misses,(unsigned long long)code_changed.load(),
        (unsigned long long)protection_events.load(),(unsigned long long)module_unloads.load(),
        stopped.load()?"true":"false",stop_control_available?"true":"false",conflict.load()?"true":"false",
        (unsigned long long)cap.started,(unsigned long long)cap.completed,(unsigned long long)cap.incomplete,
        (unsigned long long)cap.events,(unsigned long long)cap.bytes,(unsigned long long)cap.budget.rejected_captures,
        actuator==Actuator::Auto?"true":"false");
    for (unsigned i=0;i<candidate_count.load();++i) {
        const auto& c=candidates[i];
        if (i) dr_fprintf(f,",");
        dr_fprintf(f,"{\"id\":%u,\"module_offset\":%llu,\"code_hash\":\"%llx\",\"generation\":%llu,\"bytes\":%u,\"instructions\":%u,\"live_in_mask\":%u,\"live_out_mask\":%u,\"calls\":%llu,\"executions\":%llu,\"skipped_instructions\":%llu,\"guard_hits\":%llu,\"guard_misses\":%llu,\"reused_nodes\":%llu,\"dirty_nodes\":%llu,\"lifecycle\":%u,\"reason\":%u,\"active\":%s,\"sampled\":%s,\"sample_live_ins\":[",
            i+1,(unsigned long long)(c.decoded.start-c.module_start),(unsigned long long)c.decoded.code_hash,
            (unsigned long long)c.generation,
            (unsigned)c.decoded.byte_count,(unsigned)c.decoded.region.count,(unsigned)c.analysis.live_in_mask,
            (unsigned)c.analysis.live_out_mask,(unsigned long long)c.calls.load(),
            (unsigned long long)c.executions.load(),
            (unsigned long long)(c.executions.load()*c.decoded.region.count),
            (unsigned long long)c.guards.load(),(unsigned long long)c.misses.load(),
            (unsigned long long)c.reused_nodes.load(),(unsigned long long)c.dirty_nodes.load(),
            (unsigned)c.publication.lifecycle(),(unsigned)c.publication.reason(),
            (c.active.load() && c.publication.lifecycle()==arc::CandidateLifecycle::Active)?"true":"false",
            c.sample_status.load()==2?"true":"false");
        bool comma=false;
        for (unsigned r=0;r<register_count;++r) if (c.analysis.live_in_mask & (1u<<r)) {
            if (comma) dr_fprintf(f,","); comma=true;
            dr_fprintf(f,"%llu",(unsigned long long)c.sample.regs[r]);
        }
        dr_fprintf(f,"],\"raw_hex\":\"");
        for (unsigned n=0;n<c.decoded.byte_count;++n) dr_fprintf(f,"%02x",c.decoded.original_bytes[n]);
        dr_fprintf(f,"\",\"ops\":[");
        for (unsigned n=0;n<c.decoded.region.count;++n) {
            const auto& op=c.decoded.region.instructions[n];
            if (n) dr_fprintf(f,",");
            dr_fprintf(f,"{\"op\":%u,\"dst\":%u,\"a_reg\":%d,\"a\":%llu,\"b_reg\":%d,\"b\":%llu,\"scale\":%u,\"disp\":%lld}",
                (unsigned)op.op,(unsigned)op.dst,op.a.is_register?1:0,
                (unsigned long long)(op.a.is_register?(unsigned)op.a.register_id:op.a.immediate),
                op.b.is_register?1:0,
                (unsigned long long)(op.b.is_register?(unsigned)op.b.register_id:op.b.immediate),
                (unsigned)op.scale,(long long)op.displacement);
        }
        dr_fprintf(f,"]}");
    }
    dr_fprintf(f,"]}\n");
    dr_close_file(f);
}
}
DR_EXPORT void dr_client_main(client_id_t,int argc,const char* argv[]) {
    dr_set_client_name("ARC CPU backend","https://github.com/Flyingzaptop/ARC");
    parse_args(argc,argv);
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
