#include "generic_trial_storage.hpp"
#include "generic_quality_worker.hpp"
#include "arc/optimizer_quality_profile.hpp"
#include "generic_auto_session.hpp"
#include "generic_optimizer.hpp"
#include "generic_runtime.hpp"
#include "generic_gpu_profile.hpp"
#include "generic_command_mirror.hpp"
#include "generic_pixel_optimizer.hpp"
#include "generic_cpu_workers.hpp"
#include "arc/intercept_cpu_meter.hpp"
#include "arc/optimizer_session.hpp"
#include "arc/timing_evidence.hpp"
#include "arc/target_feedback.hpp"
#include "arc/profile_retry.hpp"
#include "arc/bottleneck_router.hpp"
#include "arc/compute_feedback_policy.hpp"
#include "generic_hook_control.hpp"
#include "json.hpp"
#include <atomic>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <sstream>

namespace arc::dx12::autotune {
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct State {
    std::atomic<bool> running{},cancel{};
    std::atomic<bool> observing{};
    std::mutex mutex,policy_mutex;std::condition_variable changed;
    Microsoft::WRL::ComPtr<ID3D12Device> telemetry_device;
    void* swapchain{};UINT64 last_qpc{},frames{};double frequency{};
    std::array<double,128> periods{};unsigned count{},cursor{};
    Json status{{"phase","off"}};
    std::filesystem::path directory,python,critic;
    bool feedback_only{},feedback_unavailable{};bool maximize{true};std::string quality_profile{"balanced"};
    std::unique_ptr<QualityWorker> quality_worker;
    double target{};unsigned maximum_seconds{0};
    std::atomic<double> requested_target{};
    bool capture_active{};unsigned capture_stage{};arc::PolicyBundle capture_bundle;
    std::array<optimizer::PolicyStamp,5> capture_stamps{};std::array<std::uint64_t,2> candidate_epochs{};std::uint64_t capture_evidence_frame{};double captured_work_fraction{1};
    std::array<optimizer::FrameStateSample,5> frame_states;
    Clock::time_point started{};std::uint64_t event_sequence{},journal_failures{};
    std::atomic<bool> diagnostics_enabled{};
    HWND window{};
    UINT width{},height{},surface_format{};bool fullscreen{};std::uint64_t swapchain_identity{};
    std::atomic<std::uint64_t> surface_revision{};std::uint64_t transaction_revision{},transaction_bindings{};
    std::atomic<bool> capture_invalid{};
    std::uint64_t retained_action{},retained_until{};
};
State& state(){static auto* s=new State;return *s;}
struct TrialInterrupted:std::runtime_error {using std::runtime_error::runtime_error;};
struct SurfaceChanged:TrialInterrupted {SurfaceChanged():TrialInterrupted("presentation_surface_changed") {}};
void ensure_surface(){if(state().surface_revision.load()!=state().transaction_revision)throw SurfaceChanged();if(optimizer::binding_evidence_revision()!=state().transaction_bindings)throw TrialInterrupted("texture_or_pipeline_generation_changed");}
void invalidate_surface_locked(){auto& s=state();++s.surface_revision;s.capture_invalid=true;s.capture_active=false;s.last_qpc=0;s.count=s.cursor=0;s.width=s.height=0;
    if(s.running){std::lock_guard policy_lock(s.policy_mutex);optimizer::configure(L"off");optimizer::cpu_configure(false);optimizer::sample_frame_state(false);generic::cancel_image_sequence();}s.changed.notify_all();}
void journal(Json value){
    auto& s=state();{std::lock_guard lock(s.mutex);value["present_samples"]=s.frames;}
    value["schema"]=1;value["sequence"]=++s.event_sequence;value["elapsed_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-s.started).count();
    try{const auto path=s.directory/L"events.jsonl";
        if(std::filesystem::exists(path)&&std::filesystem::file_size(path)>=8*1024*1024){
            const auto archived=[&](unsigned i){return s.directory/(L"events."+std::to_wstring(i)+L".jsonl");};
            if(std::filesystem::exists(archived(3)))std::filesystem::remove(archived(3));
            for(unsigned i=3;i>1;--i)if(std::filesystem::exists(archived(i-1)))std::filesystem::rename(archived(i-1),archived(i));
            std::filesystem::rename(path,archived(1));
        }
        std::ofstream file(path,std::ios::app);file<<value.dump()<<'\n';file.close();if(!file)++s.journal_failures;
    }catch(...){++s.journal_failures;}
}
void publish_spatial(){
    const auto text=optimizer::spatial_snapshot();if(text.empty())return;
    try{auto value=Json::parse(text);value["written_tick_ms"]=GetTickCount64();const auto temporary=state().directory/L"importance.json.tmp",path=state().directory/L"importance.json";
        std::ofstream file(temporary);file<<value.dump();file.close();if(file)MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
    }catch(...){}
}
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned n=0;for(auto c:s){if(c==L'\\'){++n;continue;}if(c==L'"'){out.append(n*2+1,L'\\');out+=c;}else{out.append(n,L'\\');out+=c;}n=0;}out.append(n*2,L'\\');return out+L'"';}
void publish(Json value){
    auto& s=state();value["control_mode"]=s.feedback_only?"target_feedback":"validated";value["image_quality_checked"]=!s.feedback_only;value["quality_profile"]=s.quality_profile;const auto activity=optimizer::activity_counters();value["observed_present_frames"]=activity[0];value["accepted_policy_submission_frames"]=activity[1];value["accepted_policy_submission_time_fraction"]=activity[2]?Json(double(activity[3])/activity[2]):Json(nullptr);value["activity_scope"]=s.feedback_only?"configured_controls_submitted_quality_not_checked":"valid_accepted_controls_submitted_not_per_pixel_gpu_effect";value["quality_limits_version"]=1;value["component_budgets_enforced"]=false;value["objective"]=s.maximize?"maximize_fps":"target_fps";
    for(const auto& text:optimizer::drain_events())try{journal(Json::parse(text));}catch(...){}
    publish_spatial();
    journal(value);{std::lock_guard lock(s.mutex);s.status=value;}
    const auto temp=s.directory/L"status.json.tmp",final=s.directory/L"status.json";
    std::ofstream file(temp);file<<value.dump(2)<<'\n';file.close();
    if(file)MoveFileExW(temp.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING);
}
UINT64 frame_number(){std::lock_guard lock(state().mutex);return state().frames;}
struct CpuWindowSample {arc::InterceptCpuMeter::Snapshot hooks;cpu_cost::Snapshot workers;UINT64 frame{};bool coherent{};};
CpuWindowSample cpu_sample(){
    CpuWindowSample sample;sample.frame=frame_number();sample.hooks=arc::InterceptCpuMeter::snapshot();sample.workers=cpu_cost::snapshot();
    sample.coherent=arc::InterceptCpuMeter::enabled()&&!sample.workers.failures&&sample.workers.live[0]>=3&&sample.frame==frame_number();return sample;
}
double cpu_window_ms(const CpuWindowSample& before,const CpuWindowSample& after){
    if(!before.coherent||!after.coherent||after.frame<=before.frame+1||after.hooks.own_ns<before.hooks.own_ns)return std::numeric_limits<double>::infinity();
    double ns=double(after.hooks.own_ns-before.hooks.own_ns);
    for(unsigned i=0;i<3;++i){if(after.workers.nanoseconds[i]<before.workers.nanoseconds[i])return std::numeric_limits<double>::infinity();ns+=double(after.workers.nanoseconds[i]-before.workers.nanoseconds[i]);}
    // Snapshots lie inside their Present intervals. Dividing by one fewer
    // interval conservatively removes the partially observed boundary frame.
    return ns/1.e6/double(after.frame-before.frame-1);
}
bool wait_frames(unsigned count){auto& s=state();std::unique_lock lock(s.mutex);const auto target=s.frames+count;
    double estimate=16.7;if(s.count){double total=0;const auto samples=std::min(32u,s.count);for(unsigned i=0;i<samples;++i)total+=s.periods[(s.cursor+s.periods.size()-1-i)%s.periods.size()];estimate=total/samples;}
    const auto timeout=std::chrono::milliseconds(std::int64_t(std::clamp(2000.+count*estimate*1.5,4000.,15000.)));
    const bool woke=s.changed.wait_for(lock,timeout,[&]{return s.cancel||s.frames>=target;});return woke&&!s.cancel;}
arc::TimingWindow period_stats(unsigned samples=32){std::lock_guard lock(state().mutex);const auto& s=state();const auto n=std::min(samples,s.count);std::array<double,128> values{};for(unsigned i=0;i<n;++i)values[i]=s.periods[(s.cursor+s.periods.size()-1-i)%s.periods.size()];return arc::timing_window({values.data(),n});}
double period(unsigned samples=32){return period_stats(samples).mean;}
bool wait_file(const std::filesystem::path& file,unsigned milliseconds=3000,bool image=false){const auto until=Clock::now()+std::chrono::milliseconds(milliseconds);while(!state().cancel&&Clock::now()<until){if(image&&state().capture_invalid)return false;if(std::filesystem::is_regular_file(file))return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;}
bool wait_restoration(bool finishing=false){const auto until=Clock::now()+std::chrono::seconds(3);while((finishing||!state().cancel)&&Clock::now()<until){if(optimizer::restoration_ready()&&mirror::restoration_ready()&&pixel::restoration_ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;}
Json read_json(const std::filesystem::path& path){if(std::filesystem::file_size(path)>2*1024*1024)throw std::runtime_error("Quality JSON capacity");std::ifstream file(path);return Json::parse(file);}
Json quality(const std::vector<std::filesystem::path>& paths,const std::filesystem::path& output,const std::function<void()>& idle={}){
    auto& s=state();Json request{{"profile",s.quality_profile},{"paths",Json::array()},{"context",{{"surface_revision",s.transaction_revision},{"captured_frame",s.capture_evidence_frame},{"candidate_epochs",s.candidate_epochs}}}};
    for(const auto& p:paths){const auto u=p.u8string();request["paths"].push_back(std::string(reinterpret_cast<const char*>(u.data()),u.size()));}
    request["frame_states"]=read_json(output.parent_path()/L"frame-state.json");
    try{
        if(!s.quality_worker)s.quality_worker=std::make_unique<QualityWorker>();
        auto result=s.quality_worker->assess(s.python,s.critic,std::move(request),[&]{return s.cancel.load();},idle);
        std::ofstream file(output);file<<result.dump(2)<<'\n';return result;
    }catch(const std::exception& e){return Json{{"accepted_quality",false},{"matched_reference",false},{"reason",e.what()},{"quality",Json::object()}};}
}
void select(const std::wstring& mode){auto& s=state();std::lock_guard lock(s.policy_mutex);
    if(s.cancel&&mode!=L"off")throw std::runtime_error("Session cancelled");
    if(!optimizer::configure(mode.c_str()))throw std::runtime_error("Optimizer policy switch refused");if(mode==L"off")optimizer::cpu_configure(false);}
void select(const arc::PolicyBundle& bundle,bool apply=true,bool trial=false){auto& s=state();const auto frame=frame_number();std::lock_guard lock(s.policy_mutex);
    if(!bundle.compute.empty()||bundle.cpu_state_cache)ensure_surface();
    if(s.cancel&&!bundle.compute.empty())throw std::runtime_error("Session cancelled");
    if(!trial&&bundle.id&&bundle.id==s.retained_action&&frame>=s.retained_until)throw TrialInterrupted("quality_evidence_expired");
    const auto until=!trial&&bundle.id==s.retained_action&&s.retained_action?s.retained_until:frame+240;
    if(!optimizer::configure_bundle(bundle,apply,0,until))throw TrialInterrupted("pipeline_or_binding_no_longer_available");}
Json calibrate_gpu(const arc::PolicyBundle& bundle,const std::filesystem::path& path){
    static std::atomic<std::uint64_t> sequence{UINT64(1)<<63};const auto epoch=sequence.fetch_add(1);
    Json result{{"epoch",epoch},{"complete_cost_evidence",false},{"samples",Json::array()}};
    if(!optimizer::control_costs_enabled()){result["reason"]="component_meter_disabled";std::ofstream file(path);file<<result.dump(2);return result;}
    select(bundle,false);if(!wait_frames(40))return result; // establish asynchronous descriptor proofs before the paired GPU diagnostic
    {std::lock_guard lock(state().policy_mutex);ensure_surface();if(state().cancel||!optimizer::begin_calibration(bundle,epoch))return result;}
    const auto first=frame_number();const bool progressed=wait_frames(6);const auto last=frame_number();
    select(L"off");const bool retired=wait_restoration();optimizer::collect();
    const auto expected=optimizer::calibration_sample_count(epoch);const auto samples=optimizer::calibration_costs(epoch);
    double sum=0;std::map<std::uint64_t,unsigned> covered;
    for(const auto& sample:samples){
        ++covered[sample.pipeline];sum+=std::max(0.0,sample.wrapped_ms-sample.original_ms)+sample.neutralize_ms+sample.upload_ms;
        result["samples"].push_back({{"pipeline",sample.pipeline},{"wrapped_ms",sample.wrapped_ms},{"original_ms",sample.original_ms},{"neutralize_ms",sample.neutralize_ms},{"upload_ms",sample.upload_ms}});
    }
    bool coverage=progressed&&retired&&expected>0&&samples.size()==expected&&last>first+1;
    for(const auto& target:bundle.compute)coverage=coverage&&covered[target.pipeline]>=2;
    result["paired_dispatch_coverage_complete"]=coverage;result["issued_samples"]=expected;result["present_intervals"]=last-first;
    result["paired_component_ms_per_frame"]=coverage?Json(sum/double(last-first-1)):Json(nullptr);
    // This is measured dispatch/guard/control cost, not yet an assertion that
    // every persistent query/capture/VRS cost or final-frame dependency is known.
    {std::ofstream file(path);file<<result.dump(2)<<'\n';}return result;
}
OptimizerTrialEvidence cpu_trial(const arc::PolicyBundle& bundle,const std::filesystem::path& path){
    OptimizerTrialEvidence evidence;evidence.action=evidence.generation=bundle.id;evidence.exact_native_state=true;
    Json runs=Json::array();const bool previous_meter=arc::InterceptCpuMeter::enabled();
    struct RestoreMeter {bool enabled;~RestoreMeter(){arc::InterceptCpuMeter::enable(enabled);}} meter{previous_meter};
    bool complete=true;double baseline=0,candidate=0,noise=0,cost=0;
    for(unsigned repeat=0;repeat<2;++repeat){
        select(L"off");if(!wait_frames(48)){complete=false;break;}const auto before_stats=period_stats();const auto before=before_stats.mean;
        select(bundle);if(!wait_frames(16)){complete=false;break;}
        arc::InterceptCpuMeter::enable(true);const auto a=cpu_sample();const auto operations_before=optimizer::cpu_cache_counters();
        if(!wait_frames(32)){complete=false;break;}const auto b=cpu_sample();
        const auto overhead=cpu_window_ms(a,b);arc::InterceptCpuMeter::enable(previous_meter);
        if(!wait_frames(40)){complete=false;break;}const auto measured_stats=period_stats();const auto measured=measured_stats.mean;const auto operations_after=optimizer::cpu_cache_counters();
        select(L"off");if(!wait_frames(40)){complete=false;break;}const auto after_stats=period_stats();const auto after=after_stats.mean;const auto reference=(before+after)*.5;
        const auto uncertainty=arc::comparison_noise(before_stats,measured_stats,after_stats);
        const bool exercised=operations_after.skipped>operations_before.skipped;
        const bool unchanged_gpu=operations_after.controlled_submissions==operations_before.controlled_submissions;
        const bool useful=reference-measured>std::max({.1,reference*.02,uncertainty});
        complete=complete&&exercised&&unchanged_gpu&&useful&&std::abs(before-after)<=reference*.1;
        baseline+=reference;candidate+=measured;noise=std::max(noise,uncertainty);cost=std::max(cost,overhead);
        runs.push_back({{"baseline_before_ms",before},{"candidate_ms",measured},{"baseline_after_ms",after},{"exact_setters_exercised",exercised},{"no_controlled_gpu_submissions",unchanged_gpu},{"hook_calls",b.hooks.calls-a.hooks.calls},{"hook_own_ns",b.hooks.own_ns-a.hooks.own_ns},{"measured_intervals",b.frame-a.frame},{"cpu_overhead_ms",std::isfinite(overhead)?Json(overhead):Json(nullptr)}});
    }
    select(L"off");evidence.restoration_confirmed=wait_restoration();evidence.complete=complete&&runs.size()==2;
    evidence.baseline_frame_ms=baseline/2;evidence.candidate_frame_ms=candidate/2;evidence.baseline_noise_ms=noise;evidence.cpu_overhead_ms=cost;
    // The structural proof is specific to a CPU-only bundle. A window with
    // cached controlled GPU work above is incomplete, never given zero cost.
    evidence.gpu_overhead_ms=evidence.complete?0:std::numeric_limits<double>::infinity();
    {std::ofstream file(path);file<<Json{{"quality_proof","exact_native_state_equivalence"},{"image_comparison_performed",false},{"runs",runs},{"complete",evidence.complete}}.dump(2)<<'\n';}
    return evidence;
}
bool capture_trial(const std::vector<std::filesystem::path>& paths,const arc::PolicyBundle& bundle,bool& observed_submission){
    struct EndSampling {~EndSampling(){optimizer::sample_frame_state(false);}} end_sampling;
    auto& s=state();select(bundle,false);if(!wait_frames(40))return false;
    optimizer::sample_frame_state(true);if(!wait_frames(2)){optimizer::sample_frame_state(false);return false;}
    {std::lock_guard lock(s.mutex);ensure_surface();if(s.cancel)return false;s.capture_invalid=false;s.capture_bundle=bundle;s.capture_stage=0;s.capture_stamps={};s.frame_states={};s.candidate_epochs={};
        s.capture_active=generic::request_image_sequence(paths,reinterpret_cast<IDXGISwapChain*>(s.swapchain));
        if(!s.capture_active){optimizer::sample_frame_state(false);return false;}}
    bool complete=true;for(const auto& path:paths)complete=wait_file(path,4000,true)&&complete;ensure_surface();
    Json provenance,state_samples=Json::array();
    {std::lock_guard lock(s.mutex);s.capture_active=false;
        observed_submission=true;
        for(unsigned i=0;i<2;++i){const auto& a=s.capture_stamps[i*2];const auto& b=s.capture_stamps[i*2+1];const auto& c=s.capture_stamps[i*2+2];
            observed_submission=observed_submission&&s.candidate_epochs[i]&&b.epoch==s.candidate_epochs[i]&&b.last_active_epoch==s.candidate_epochs[i]&&b.active_submissions>a.active_submissions&&c.active_submissions==b.active_submissions;}
        provenance={{"kind","shader_written_epoch_on_present_queue"},{"candidate_epochs",s.candidate_epochs},{"cpu_submission_observed",observed_submission},{"same_queue_gpu_execution_proven",false}};
        for(const auto& sample:s.frame_states)state_samples.push_back({{"pipeline",sample.pipeline},{"submission",sample.submission},{"keys",sample.keys},{"words",sample.words},{"valid",sample.valid}});}
    optimizer::sample_frame_state(false);
    bool gpu_proven=complete&&observed_submission,spatial_effect=true;std::set<std::uint64_t> executed;std::map<std::uint64_t,std::uint64_t> coarse_tiles,evaluated_tiles;s.captured_work_fraction=1;
    if(gpu_proven)try{for(unsigned image=0;image<2;++image){executed.clear();coarse_tiles.clear();evaluated_tiles.clear();const auto captured=read_json(paths[image*2+1]);const auto& proof=captured.at("gpu_execution");
        gpu_proven=gpu_proven&&proof.value("kind",std::string{})=="shader_written_epoch"&&proof.value("image_fence_completed",false)&&proof.value("capture_queue",0ull)!=0;
        for(const auto& marker:proof.at("markers")){
            const auto epoch=marker.value("expected_epoch",0ull),pipeline=marker.value("expected_pipeline",0ull);
            if(epoch!=s.candidate_epochs[image])continue;
            const bool matched=marker.value("fence_completed",false)&&marker.value("actual_epoch",0ull)==epoch&&marker.value("actual_pipeline",0ull)==pipeline&&marker.value("queue",0ull)==proof.at("capture_queue").get<std::uint64_t>();
            gpu_proven=gpu_proven&&matched;if(matched){executed.insert(pipeline);coarse_tiles[pipeline]+=marker.value("coarse_tiles",0ull);evaluated_tiles[pipeline]+=marker.value("evaluated_tiles",0ull);}
        }
        for(const auto& target:bundle.compute){gpu_proven=gpu_proven&&executed.contains(target.pipeline);if(target.protect_edges&&(target.x_rate>1||target.y_rate>1||target.comparison_taps||target.mip_steps)){spatial_effect=spatial_effect&&coarse_tiles[target.pipeline]>0;s.captured_work_fraction=std::min(s.captured_work_fraction,evaluated_tiles[target.pipeline]?double(coarse_tiles[target.pipeline])/evaluated_tiles[target.pipeline]:0.);}}
        provenance["captures"].push_back(proof);
        }
    }catch(...){gpu_proven=false;}
    observed_submission=gpu_proven&&spatial_effect;provenance["same_queue_gpu_execution_proven"]=gpu_proven;provenance["gpu_execution_confirmed"]=gpu_proven;provenance["effective_spatial_work"]=spatial_effect;
    {std::ofstream file(paths[0].parent_path()/L"submission-provenance.json");file<<provenance.dump(2)<<'\n';}
    {std::ofstream file(paths[0].parent_path()/L"frame-state.json");file<<state_samples.dump(2)<<'\n';}
    select(L"off");return complete;
}
#include "generic_target_feedback.inl"
DWORD WINAPI run(void*){
    cpu_cost::Registration worker_cpu;
    const bool meter_was_enabled=arc::InterceptCpuMeter::enabled();
    struct RestoreMeter {bool enabled;~RestoreMeter(){arc::InterceptCpuMeter::enable(enabled);}} restore_meter{meter_was_enabled};
    auto& s=state();SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);const auto started=Clock::now();
    placement::configure(L"normal"); // one decision loop; no concurrent affinity experiment
    try{
        if(s.feedback_only){run_target_feedback();const bool restored=wait_restoration(true);publish({{"phase",restored?"stopped":"faulted"},{"reason",s.feedback_unavailable?"unsupported_profile_runtime_stopped":"stop_requested_or_session_limit"},{"restoration_confirmed",restored}});if(restored)hooks::request_passive_when_idle();s.running=false;return 0;}
        arc::OptimizerSessionConfig config;config.target_fps=s.target;config.maximize_fps=s.maximize;config.enforce_component_budgets=false;config.require_gpu_execution=true;config.warmup_samples=32;config.settle_samples=8;config.hold_samples=120;
        const auto limits=arc::optimizer_quality_limits(s.quality_profile);config.min_ssim=limits.ssim;config.max_mean_error=limits.mean;config.max_tile_p99=limits.p99;config.max_worst_tile=limits.worst;config.max_temporal_p99=limits.temporal;config.require_local_temporal_quality=true;
        optimizer::spatial_learning(true,s.quality_profile=="aggressive"?.08f:.02f);
        arc::OptimizerSession policy(config);
        struct Choice {arc::PolicyBundle bundle;double cost{};bool cpu{};unsigned mode{};std::uint64_t pipeline{};double exploration{1};bool revalidation{};};
        struct Tuning {float good{},bad{2},current{.25f};unsigned rounds{};bool failed{};};
        const auto cpu_not_before=Clock::now()+std::chrono::seconds(15);
        std::vector<Choice> choices;Choice verified;UINT64 last_probe_frames=256,last_quality_frames=256;arc::PolicyBundle incumbent;std::uint64_t next_choice=0;
        std::filesystem::path best_images,last_rejected_images,worst_images;double worst_image_error=-1;
        struct ProbeHistory {double milliseconds{},fraction{.25};unsigned count{};};std::map<std::pair<std::uint64_t,unsigned>,ProbeHistory> probe_history;
        std::map<std::uint64_t,std::optional<arc::ComputePolicy>> proposals;
        std::map<std::uint64_t,std::uint64_t> cpu_choices;
        std::map<std::string,std::uint64_t> gpu_choices;
        std::map<std::pair<std::uint64_t,unsigned>,Tuning> tuning;
        std::map<std::uint64_t,std::uint64_t> retry_after;
        std::vector<optimizer::WorkCandidate> catalog;std::map<std::pair<std::uint64_t,bool>,std::filesystem::path> cost_diagnostics;std::set<std::uint64_t> cpu_diagnostics;
        auto refresh_candidates=[&]{
            std::vector<SessionAction> actions;
            const auto frame=frame_number();
            for(const auto& choice:choices)actions.push_back({choice.bundle.id,choice.bundle.id,0,choice.exploration,(!choice.cpu||Clock::now()>=cpu_not_before)&&(!retry_after.contains(choice.bundle.id)||frame>=retry_after[choice.bundle.id]),choice.cost,false,choice.cpu&&choice.bundle.compute.empty(),choice.revalidation});
            if(incumbent.id&&std::none_of(choices.begin(),choices.end(),[&](const auto& c){return c.bundle.id==incumbent.id;}))actions.push_back({incumbent.id,incumbent.id,0,1,true,0,false,incumbent.compute.empty(),false});
            policy.candidates(std::move(actions));
        };
        auto rebuild_choices=[&]{
            choices.clear();
            if(!incumbent.cpu_state_cache){auto& id=cpu_choices[incumbent.id];if(!id)id=++next_choice;auto cpu=incumbent;cpu.id=id;cpu.cpu_state_cache=true;choices.push_back({std::move(cpu),0,true});}
            for(const auto& work:catalog){const auto& c=work.capabilities;
                for(unsigned mode=0;mode<31u;++mode){
                    if(s.quality_profile!="aggressive"&&((mode>=11&&mode<=17)||mode>=27))continue;
                    const bool ready[]{c.zero,c.comparison,c.coarse&&c.edges,c.coarse&&c.edges,c.coarse&&c.edges,c.mips,c.mips,c.mips,c.coarse,c.coarse,c.coarse,c.coarse&&c.edges,c.coarse&&c.edges,c.coarse&&c.edges,c.mips,c.mips,c.mips,c.mips,c.mips,c.samples,c.samples,c.samples,c.comparison,c.mips,c.mips,c.mips,c.mips,c.mips,c.mips,c.mips,c.mips};
                    if(!ready[mode])continue;
                    arc::ComputePolicy setting;if(const auto* current=incumbent.find(c.pipeline))setting=*current;setting.pipeline=c.pipeline;
                    if(mode==0)setting.zero_factor=1;
                    else if(mode==1)setting.comparison_taps=9;
                    else if(mode<=4){auto& search=tuning[{c.pipeline,mode}];if(search.rounds>=8)continue;setting.x_rate=mode==2?1:2;setting.y_rate=mode==3?1:2;setting.protect_edges=true;setting.edge_threshold=search.current;}
                    else if(mode<=7)setting.mip_steps=mode==5?1:mode==6?2:4;
                    else if(mode<=10){setting.x_rate=mode==8?1:2;setting.y_rate=mode==9?1:2;setting.protect_edges=false;}
                    else if(mode<=13){auto& search=tuning[{c.pipeline,mode}];if(search.rounds>=8)continue;setting.x_rate=mode==11?2:4;setting.y_rate=mode==12?2:4;setting.protect_edges=true;setting.edge_threshold=search.current;}
                    else if(mode<=18)setting.mip_steps=mode==18?3:mode-9;
                    else if(mode<=21){setting.sample_percent=mode==19?75:mode==20?50:25;setting.protect_edges=false;}
                    else if(mode==22){setting.comparison_taps=9;setting.protect_edges=false;}
                    else{setting.mip_steps=mode-22;setting.protect_edges=false;}
                    if(setting.sample_percent==100&&c.edges&&mode>=1&&(mode<=7||(mode>=14&&mode<=18))){auto& search=tuning[{c.pipeline,mode}];if(search.rounds>=8)continue;setting.protect_edges=true;setting.edge_threshold=search.current;}
                    if(const auto* current=incumbent.find(c.pipeline);current&&*current==setting)continue;
                    auto bundle=incumbent;
                    if(bundle.replace(setting)){
                        auto ordered=bundle.compute;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.pipeline<b.pipeline;});
                        std::ostringstream key;key<<bundle.cpu_state_cache<<std::hexfloat;
                        for(const auto& p:ordered)key<<':'<<p.pipeline<<','<<p.x_rate<<','<<p.y_rate<<','<<p.comparison_taps<<','<<p.zero_factor<<','<<p.mip_steps<<','<<p.protect_edges<<','<<p.edge_threshold<<','<<p.sample_percent;
                        auto found=gpu_choices.find(key.str());if(found==gpu_choices.end()){if(gpu_choices.size()>=4096)continue;found=gpu_choices.emplace(key.str(),++next_choice).first;}
                        bundle.id=found->second;// Rank measured pass expense by a work-reduction upper bound,
                        // discounted by completed probes. This is an exploration
                        // priority, never evidence of achieved speedup.
                        const double opportunity=mode==22?.64:mode>=23?.15:mode>=19?(mode-18)*.25:mode>=11&&mode<=13?(mode==13?.9375:.875):mode>=14?.25:mode==0?.25:mode==1?.64:mode==4||mode==10?.75:mode==2||mode==3||mode==8||mode==9?.5:.15;
                        const auto exploration=((mode>=8&&mode<=10?1.25:1.0)+(mode>=1&&(mode<=7||mode>=11)?tuning[{c.pipeline,mode}].rounds*.75:0))/opportunity;
                        const auto history=probe_history.find({c.pipeline,mode});const double trial_ms=history!=probe_history.end()?history->second.milliseconds:3500.+(152+(setting.protect_edges?128:0))*std::max(1.,period());
                        const double fraction=setting.protect_edges?(history!=probe_history.end()?std::max(.02,history->second.fraction):.25):1.;
                        choices.push_back({std::move(bundle),work.gpu_ms_per_window,false,mode,c.pipeline,exploration*std::max(.1,trial_ms/5000)/fraction});
                    }
                }
            }
            for(const auto& work:catalog){const auto pipeline=work.capabilities.pipeline;if(!proposals.contains(pipeline))proposals[pipeline]=optimizer::performance_proposal(pipeline,s.quality_profile);
                if(const auto& saved=proposals[pipeline];saved&&(!incumbent.find(pipeline)||*incumbent.find(pipeline)!=*saved)){auto bundle=incumbent;bundle.id=1;if(bundle.replace(*saved)&&bundle.valid()){const auto same=std::find_if(choices.begin(),choices.end(),[&](const auto& c){const auto* recipe=c.bundle.find(pipeline);return recipe&&*recipe==*saved;});if(same!=choices.end())same->exploration=.01;else{const auto proposal_id="proposal:"+std::to_string(pipeline)+":"+std::to_string(incumbent.id);auto& id=gpu_choices[proposal_id];if(!id)id=++next_choice;bundle.id=id;choices.push_back({std::move(bundle),work.gpu_ms_per_window,false,100,pipeline,.01});}}}}
            // Different capability routes can yield the same complete policy
            // (e.g. a pass without spatial inputs). One action identity only.
            {std::set<std::uint64_t> unique;std::erase_if(choices,[&](const auto& choice){return !unique.insert(choice.bundle.id).second;});}
            std::stable_sort(choices.begin(),choices.end(),[](const auto& a,const auto& b){return a.cost/(.01+a.exploration)>b.cost/(.01+b.exploration);});if(choices.size()>126)choices.resize(126);
            if(verified.bundle.id&&std::all_of(verified.bundle.compute.begin(),verified.bundle.compute.end(),[&](const auto& target){return std::any_of(catalog.begin(),catalog.end(),[&](const auto& work){return work.capabilities.pipeline==target.pipeline;});})){
                std::erase_if(choices,[&](const auto& choice){return choice.bundle.id==verified.bundle.id;});choices.insert(choices.begin(),verified);
            }
            refresh_candidates();
        };
        rebuild_choices();
        UINT64 last=0,trial=0,profile=0,catalog_check=0,last_bindings=optimizer::binding_evidence_revision();std::uint64_t retained=0,last_surface=s.surface_revision;
        std::string idle_phase;auto last_discovery=Clock::now(),last_telemetry=Clock::now(),last_diagnostics=Clock::now();double scene_reference=0;unsigned scene_drift=0;
        publish({{"phase","warmup"},{"target_fps",s.target},{"quality_reference","live_motion_qualified"}});
        while(!s.cancel){
            if(const auto requested=s.requested_target.exchange(0);requested>0){s.target=requested;policy.target(requested);publish({{"phase","target_updated"},{"target_fps",s.target}});}
            if(s.maximum_seconds&&std::chrono::duration<double>(Clock::now()-started).count()>s.maximum_seconds)break;
            if(!wait_frames(1)){if(s.cancel)break;idle_phase.clear();select(L"off");const auto reset=policy.scene_changed();if(reset.kind==SessionRequestKind::Restore)policy.restored(wait_restoration());incumbent={};retained=0;s.retained_action=s.retained_until=0;scene_reference=0;tuning.clear();retry_after.clear();rebuild_choices();publish({{"phase","inactive"},{"reason","no_present_progress"}});continue;}
            const auto now=frame_number();if(now==last)continue;const auto elapsed_frames=now-last;last=now;
            s.transaction_revision=s.surface_revision.load();s.transaction_bindings=optimizer::binding_evidence_revision();const bool changed_bindings=s.transaction_bindings!=last_bindings;last_bindings=s.transaction_bindings;const bool changed_surface=s.transaction_revision!=last_surface;last_surface=s.transaction_revision;
            if(s.diagnostics_enabled&&Clock::now()-last_diagnostics>=std::chrono::milliseconds(500)){last_diagnostics=Clock::now();optimizer::request_spatial_diagnostics();publish_spatial();}
            if(Clock::now()-last_telemetry>=std::chrono::seconds(2)){
                for(const auto& text:optimizer::drain_events())try{journal(Json::parse(text));}catch(...){}
                last_telemetry=Clock::now();PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
                const bool available=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=FALSE;
                const auto allocations=optimizer::control_allocation_bytes();const auto images=generic::image_allocation_bytes();
                const auto children=cpu_cost::memory_snapshot();
                journal({{"phase","telemetry"},{"frame_ms",period()},{"game_process_memory_available",available},{"game_process_private_bytes",available?Json(memory.PrivateUsage):Json(nullptr)},{"game_process_working_set_bytes",available?Json(memory.WorkingSetSize):Json(nullptr)},{"memory_scope","whole_game_process_not_arc_allocation"},{"arc_control_default_heap_bytes",allocations[0]},{"arc_control_upload_heap_bytes",allocations[1]},{"arc_control_readback_heap_bytes",allocations[2]},{"arc_image_default_heap_bytes",images[0]},{"arc_image_readback_heap_bytes",images[2]},{"arc_allocation_coverage","control_spatial_and_image_buffers_excludes_driver_PSO_query_storage"},{"arc_children_private_bytes",children.private_bytes},{"arc_children_sampled_peak_private_bytes",children.sampled_peak_private_bytes},{"arc_children_peak_working_set_bytes",children.peak_working_set_bytes},{"arc_children_memory_failures",children.failures},{"arc_children_memory_samples",children.samples},{"arc_children_kind_order",{"worker","compiler","critic"}}});
            }
            // Receiving Presents is not evidence that shader creation was
            // observed. A late attach cannot reconstruct opaque root/PSO
            // objects. Do not spend time on unrelated automatic trials while
            // all compute work lacks the metadata needed by this optimizer.
            const auto coverage=optimizer::connection_coverage();
            if(now>=32&&coverage.unknown_root_dispatches&&!coverage.pipelines){
                if(idle_phase!="render_metadata_missing"){
                    select(L"off");idle_phase="render_metadata_missing";
                    publish({{"phase",idle_phase},{"reason","unobserved_root_signatures_or_compute_pipelines"},{"target_fps",s.target},{"active_action",0},{"restart_recommended",true}});
                }
                continue;
            }
            // Compilation is asynchronous: a profile can finish before its
            // expensive pipeline is ready. Refresh bounded discovery without
            // inventing new action IDs on every frame or repeating rejections.
            if(profile&&now-catalog_check>=120){
                catalog_check=now;auto next=optimizer::candidate_catalog();
                const bool same=next.size()==catalog.size()&&std::equal(next.begin(),next.end(),catalog.begin(),[](const auto& a,const auto& b){return a.capabilities.pipeline==b.capabilities.pipeline&&a.profile_session==b.profile_session;});
                if(!same){catalog=std::move(next);rebuild_choices();}else refresh_candidates();
            }
            const auto current_period=period();
            if(!scene_reference)scene_reference=current_period;
            scene_drift=(current_period>scene_reference*1.75||current_period<scene_reference*.55)?scene_drift+1:0;
            auto request=(changed_surface||changed_bindings||scene_drift>=8)?policy.scene_changed():policy.frame(current_period,true,elapsed_frames,!retained||now+last_probe_frames+64<s.retained_until);
            if(changed_surface){proposals.clear();probe_history.clear();}
            if(changed_surface||changed_bindings||scene_drift>=8){if(request.kind!=SessionRequestKind::Restore)optimizer::reset_binding_evidence();tuning.clear();retry_after.clear();rebuild_choices();}
            if(policy.snapshot().phase==SessionPhase::Faulted)throw std::runtime_error("Policy restoration not confirmed");
            if(request.kind==SessionRequestKind::None&&retained&&now+std::max<UINT64>(128,last_quality_frames)+32>=s.retained_until&&now<s.retained_until)request=policy.revalidate();
            if(changed_bindings)journal({{"phase","binding_evidence_invalidated"},{"binding_revision",last_bindings}});
            if(changed_surface){if(s.quality_worker&&s.quality_worker->unavailable())s.quality_worker.reset();journal({{"phase","surface_changed"},{"surface_revision",last_surface}});scene_reference=current_period;}
            if(scene_drift>=8){scene_reference=current_period;scene_drift=0;}
            if(request.kind==SessionRequestKind::None&&policy.snapshot().phase==SessionPhase::Limited&&!policy.snapshot().active_action&&Clock::now()-last_discovery>std::chrono::seconds(15))request={SessionRequestKind::Profile,0};
            if(request.kind==SessionRequestKind::None){const auto current=policy.snapshot();
                if(current.phase==SessionPhase::Active||current.phase==SessionPhase::Limited){
                    const std::string phase=current.phase==SessionPhase::Limited&&!current.active_action?"limited":!s.maximize&&current.filtered_frame_ms<=current.target_frame_ms?"target_met":"holding";
                    if(phase!=idle_phase){idle_phase=phase;publish({{"phase",phase},{"target_fps",s.target},{"filtered_frame_ms",current.filtered_frame_ms},{"active_action",current.active_action},{"accepted",current.accepted},{"rejected",current.rejected}});}
                }
            }else idle_phase.clear();
            if(request.kind==SessionRequestKind::Profile){
                last_discovery=Clock::now();select(L"off");const auto path=s.directory/(L"profile-"+std::to_wstring(++profile)+L".json");
                if(!gpu_profile::request(path.wstring(),16)||!wait_file(path,12000)){
                    gpu_profile::stop();const auto reset=policy.scene_changed();if(reset.kind==SessionRequestKind::Restore)policy.restored(wait_restoration());
                    incumbent={};retained=0;s.retained_action=s.retained_until=0;catalog.clear();rebuild_choices();
                    publish({{"phase","discovery_deferred"},{"reason","gpu_profile_incomplete"},{"frames_progressed",frame_number()-now},{"retry_after_frames",120}});
                    wait_frames(120);continue;
                }
                catalog=optimizer::candidate_catalog();rebuild_choices();
                publish({{"phase","discovery_complete"},{"target_fps",s.target},{"measured_work_candidates",catalog.size()},{"policy_candidates",choices.size()}});
            }else if(request.kind==SessionRequestKind::Probe){
                if(s.quality_worker&&s.quality_worker->unavailable()){
                    select(L"off");const auto reset=policy.scene_changed();if(reset.kind==SessionRequestKind::Restore)policy.restored(wait_restoration());incumbent={};retained=0;s.retained_action=s.retained_until=0;
                    publish({{"phase","quality_unavailable"},{"reason","quality_worker_restart_limit"}});wait_frames(60);continue;
                }
                const auto probe_started=frame_number();const auto probe_wall_started=Clock::now();
                try{
                const auto dir=s.directory/(L"trial-"+std::to_wstring(++trial));
                UINT image_width{},image_height{};{std::lock_guard lock(s.mutex);image_width=s.width;image_height=s.height;}
                if(!reserve_trial_storage(s.directory,std::uint64_t(image_width)*image_height*20,{best_images,last_rejected_images,worst_images}))throw TrialInterrupted("trial_image_quota");
                std::filesystem::create_directory(dir);
                const auto choice=std::find_if(choices.begin(),choices.end(),[&](const auto& c){return c.bundle.id==request.action;});
                if(choice==choices.end())throw std::runtime_error("Unknown bundle request");
                const auto candidate=choice->bundle;const auto generation=candidate.id;const auto candidate_mode=choice->mode;const auto candidate_pipeline=choice->pipeline?choice->pipeline:candidate.compute.empty()?0:candidate.compute.front().pipeline;const auto choice_cost=choice->cost;const auto choice_cpu=choice->cpu;const bool revalidation=choice->revalidation;
                publish({{"phase","trial_started"},{"trial",trial},{"candidate_action",candidate.id},{"active_action",retained},{"pipeline",candidate_pipeline},{"revalidation",revalidation},{"evidence_directory",dir.string()}});
                if(!candidate.compute.empty()){
                    void* swap{};{std::lock_guard lock(s.mutex);swap=s.swapchain;}
                    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;queue.Attach(generic::acquire_presentation_queue(reinterpret_cast<IDXGISwapChain*>(swap)));
                    if(!queue)throw std::runtime_error("Presentation queue unavailable for GPU trial");optimizer::require_presentation_queue(queue.Get());
                }
                if(choice->cpu&&candidate.compute.empty()){
                    auto evidence=cpu_trial(candidate,dir/L"cpu-trial.json");if(s.cancel)break;
                    if(const auto requested=s.requested_target.exchange(0);requested>0){s.target=requested;policy.target(requested);}
                    ensure_surface();const auto decision=policy.evidence(evidence);
                if(decision.kind==SessionRequestKind::Apply){if(!candidate.compute.empty()&&!optimizer::seal_binding_evidence(candidate.id))throw TrialInterrupted("binding_evidence_not_sealable");s.retained_action=decision.action;s.retained_until=frame_number()+600;optimizer::approve_policy(decision.action,s.retained_until);select(candidate);incumbent=candidate;for(const auto& setting:candidate.compute)optimizer::remember_performance_proposal(setting,s.quality_profile);verified={candidate,choice_cost,choice_cpu,candidate_mode,candidate_pipeline,.05,true};retained=decision.action;policy.applied(retained,true);last=frame_number();rebuild_choices();}
                    const auto current=policy.snapshot();Json report{{"phase","trial_finished"},{"trial",trial},{"action",request.action},{"kind","exact_cpu_state"},{"accepted",current.accepted},{"rejected",current.rejected},{"retained",retained},{"baseline_frame_ms",evidence.baseline_frame_ms},{"candidate_frame_ms",evidence.candidate_frame_ms},{"cpu_overhead_ms",std::isfinite(evidence.cpu_overhead_ms)?Json(evidence.cpu_overhead_ms):Json(nullptr)},{"restoration_confirmed",evidence.restoration_confirmed}};
                    {std::ofstream file(dir/L"decision.json");file<<report.dump(2)<<'\n';}publish(std::move(report));continue;
                }
                struct ProbePause {~ProbePause(){optimizer::end_spatial_training();optimizer::pause_spatial_probes(state().cancel.load());}} probe_pause;
                if(!revalidation&&optimizer::begin_spatial_training(candidate,candidate_pipeline)){
                    select(candidate,false);unsigned waited=0;optimizer::SpatialProgress progress;
                    do{if(!wait_frames(32))throw TrialInterrupted("spatial_training_interrupted");optimizer::collect();waited+=32;progress=optimizer::spatial_progress();}while(waited<384&&(!progress.models||progress.sampled<progress.models||progress.eligible_fraction<.3));
                    journal({{"phase","spatial_training"},{"pipeline",candidate_pipeline},{"frames",waited},{"models",progress.models},{"sampled",progress.sampled},{"eligible",progress.eligible},{"eligible_fraction",progress.eligible_fraction},{"probes",progress.probes},{"best_error",progress.best_error}});
                    optimizer::end_spatial_training();
                }
                optimizer::pause_spatial_probes(true);
                const auto cost_key=std::make_pair(candidate_pipeline,candidate.find(candidate_pipeline)->protect_edges);
                Json gpu_calibration{{"complete_cost_evidence",false}};
                if(!cost_diagnostics.contains(cost_key)){gpu_calibration=calibrate_gpu(candidate,dir/L"gpu-calibration.json");cost_diagnostics[cost_key]=dir/L"gpu-calibration.json";gpu_calibration["measured_this_trial"]=true;}
                else{gpu_calibration["measured_this_trial"]=false;gpu_calibration["reason"]="component_diagnostic_already_sampled_net_timing_still_required";gpu_calibration["previous_evidence"]=cost_diagnostics[cost_key].string();}
                if(retained&&frame_number()<s.retained_until)select(incumbent);else select(L"off");if(!revalidation&&!wait_frames(40))break;double baseline_ms=period();
                // The final-image reference always uses the ORIGINAL policy,
                // even when comparing a replacement against a retained setting.
                const std::vector<std::filesystem::path> images{dir/L"before.json",dir/L"candidate.json",dir/L"after.json",dir/L"candidate-2.json",dir/L"after-2.json"};
                bool submitted=false;if(!capture_trial(images,candidate,submitted))throw TrialInterrupted("consecutive_reference_capture_unavailable");
                // Freeze the generations covered by the image transaction.
                // Later timing windows must not silently learn replacement textures.
                if(submitted&&!optimizer::seal_binding_evidence(candidate.id))throw TrialInterrupted("captured_binding_evidence_not_sealable");
                // No observed active submission cannot establish an effect.
                // Avoid spending a CPU-heavy image trial on unchanged work.
                if(!wait_restoration())throw TrialInterrupted("image_policy_retirement_unconfirmed");
                if(retained&&frame_number()<s.retained_until)select(incumbent);
                double candidate_ms=baseline_ms,baseline_before_ms=baseline_ms,baseline_after_ms=baseline_ms;bool timing_stable=false;arc::TimingWindow before_stats,candidate_stats,after_stats;
                double cpu_overhead_ms=std::numeric_limits<double>::infinity();UINT64 cpu_frames=0;double original_ms=0;unsigned timing_samples=0;bool reference_incumbent=false;Json timing_attempts=Json::array();
                int renewal_stage=0;UINT64 renewal_deadline{};bool renewal_complete=false;
                const bool overlap_renewal=revalidation&&retained&&candidate.id==incumbent.id&&frame_number()+72<s.retained_until;
                auto renewal_tick=[&]{
                    if(!overlap_renewal||renewal_stage<0||renewal_stage==6)return;ensure_surface();const auto now=frame_number();
                    if(renewal_stage==0){select(L"off");renewal_stage=1;return;}
                    if(renewal_stage==1){if(optimizer::restoration_ready()){renewal_deadline=now+32;renewal_stage=2;}return;}
                    if(renewal_stage==2&&now>=renewal_deadline){before_stats=period_stats();baseline_before_ms=before_stats.mean;
                        if(now+36>=s.retained_until){renewal_stage=-1;if(now<s.retained_until)select(incumbent);return;}
                        select(incumbent);renewal_deadline=now+32;renewal_stage=3;return;}
                    if(renewal_stage==3){if(now>=s.retained_until){select(L"off");renewal_stage=-1;return;}if(now>=renewal_deadline){candidate_stats=period_stats();candidate_ms=candidate_stats.mean;select(L"off");renewal_stage=4;}return;}
                    if(renewal_stage==4){if(optimizer::restoration_ready()){renewal_deadline=now+32;renewal_stage=5;}return;}
                    if(renewal_stage==5&&now>=renewal_deadline){after_stats=period_stats();baseline_after_ms=after_stats.mean;baseline_ms=(baseline_before_ms+baseline_after_ms)*.5;original_ms=baseline_ms;timing_samples=32;
                        timing_stable=std::isfinite(baseline_ms)&&baseline_ms>0&&std::abs(baseline_before_ms-baseline_after_ms)<=baseline_ms*.1;
                        timing_attempts.push_back({{"samples",32},{"before_ms",baseline_before_ms},{"candidate_ms",candidate_ms},{"after_ms",baseline_after_ms},{"noise_ms",arc::comparison_noise(before_stats,candidate_stats,after_stats)},{"reference","original"},{"critic_running",true}});
                        renewal_complete=true;renewal_stage=6;if(now<s.retained_until)select(incumbent);
                    }
                };
                const auto quality_started=Clock::now();
                auto judgement=submitted?quality(images,dir/L"quality.json",renewal_tick):Json{{"accepted_quality",false},{"matched_reference",false},{"reason","no_verified_effective_policy_work"},
                    {"quality",{{"ssim_gaussian_luma",nullptr},{"mean_linear_rgb_error",nullptr},{"p99_tile_linear_rgb_error",nullptr}}}};
                const auto quality_wall_ms=std::chrono::duration<double,std::milli>(Clock::now()-quality_started).count();
                if(revalidation&&submitted&&judgement.value("matched_reference",false)&&!judgement.value("accepted_quality",false)){s.retained_until=0;optimizer::approve_policy(0,0);select(L"off");publish({{"phase","quality_revoked"},{"active_action",0},{"reason","confirmed_incumbent_quality_failure"},{"trial",trial}});}
                last_quality_frames=std::clamp<UINT64>(frame_number()-s.capture_evidence_frame,64,480);
                if(s.cancel)break;
                if(submitted&&judgement.value("accepted_quality",false)&&!renewal_complete){
                    for(const unsigned samples:{32u,64u,128u}){
                        const auto now=frame_number();const unsigned candidate_settle=std::any_of(candidate.compute.begin(),candidate.compute.end(),[](const auto& p){return p.protect_edges;})?40:8;const auto timing_frames=3*samples+16+candidate_settle+(meter_was_enabled&&!cpu_diagnostics.contains(candidate_pipeline)?32:0);if(now-s.capture_evidence_frame+timing_frames+8>=600)break;
                        timing_samples=samples;reference_incumbent=retained&&!revalidation&&now+timing_frames+64<s.retained_until;
                        const auto profile_timing=[&](const wchar_t* label){if(optimizer::control_costs_enabled()){const auto path=dir/(std::wstring(L"timing-")+label+L"-"+std::to_wstring(samples)+L".json");gpu_profile::request(path.wstring(),16);}};
                        if(reference_incumbent)select(incumbent);else select(L"off");profile_timing(L"before");if(!wait_frames(samples+8))break;before_stats=period_stats(samples);baseline_before_ms=before_stats.mean;
                        select(candidate,true,true);profile_timing(L"candidate");
                        if(meter_was_enabled&&!cpu_diagnostics.contains(candidate_pipeline)){
                            const auto cost_before=cpu_sample();if(!wait_frames(32))break;const auto cost_after=cpu_sample();cpu_overhead_ms=cpu_window_ms(cost_before,cost_after);cpu_frames=cost_after.frame-cost_before.frame;cpu_diagnostics.insert(candidate_pipeline);
                        }
                        if(!wait_frames(samples+candidate_settle))break;candidate_stats=period_stats(samples);candidate_ms=candidate_stats.mean;
                        select(L"off");if(!wait_restoration())throw TrialInterrupted("timing_policy_retirement_unconfirmed");
                        if(reference_incumbent)select(incumbent);else select(L"off");profile_timing(L"after");if(!wait_frames(samples+8))break;after_stats=period_stats(samples);baseline_after_ms=after_stats.mean;
                        baseline_ms=(baseline_before_ms+baseline_after_ms)*.5;timing_stable=std::isfinite(baseline_ms)&&baseline_ms>0&&std::abs(baseline_before_ms-baseline_after_ms)<=baseline_ms*.1;
                        const auto noise=arc::comparison_noise(before_stats,candidate_stats,after_stats),gain=baseline_ms-candidate_ms,required=std::max(.1,baseline_ms*.02);
                        timing_attempts.push_back({{"samples",samples},{"before_ms",baseline_before_ms},{"candidate_ms",candidate_ms},{"after_ms",baseline_after_ms},{"noise_ms",noise},{"reference",reference_incumbent?"incumbent":"original"}});
                        if(!reference_incumbent)original_ms=baseline_ms;
                        if(timing_stable&&(gain>std::max(required,noise)||gain+noise<required))break;
                    }
                    if(reference_incumbent&&timing_stable){select(L"off");if(!wait_restoration()||!wait_frames(40))throw TrialInterrupted("original_timing_unavailable");original_ms=period();}
                }
                select(L"off");if(!wait_restoration())throw std::runtime_error("GPU policy retirement not confirmed");
                const bool restored_original=true;
                if(retained&&frame_number()<s.retained_until)select(incumbent);else select(L"off");if(!wait_frames(8))break;
                {auto& history=probe_history[{candidate_pipeline,candidate_mode}];const auto duration=std::chrono::duration<double,std::milli>(Clock::now()-probe_wall_started).count();history.milliseconds=history.count?history.milliseconds*.5+duration*.5:duration;history.fraction=s.captured_work_fraction;++history.count;}
                const auto& q=judgement.at("quality");OptimizerTrialEvidence evidence;
                evidence.action=request.action;evidence.generation=generation;evidence.complete=submitted&&timing_stable;evidence.restoration_confirmed=restored_original;evidence.matched_reference=submitted&&judgement.value("matched_reference",false);
                evidence.original_frame_ms=original_ms;evidence.baseline_frame_ms=baseline_ms;evidence.candidate_frame_ms=candidate_ms;evidence.baseline_noise_ms=arc::comparison_noise(before_stats,candidate_stats,after_stats);
                const auto metric=[&](const char* name){return q.contains(name)&&q.at(name).is_number()?q.at(name).get<double>():std::numeric_limits<double>::quiet_NaN();};
                evidence.ssim=metric("ssim_gaussian_luma");evidence.mean_error=metric("mean_linear_rgb_error");evidence.tile_p99=metric("p99_tile_linear_rgb_error");
                // CPU covers hooks, lifetime callbacks, owned threads and live
                // children. GPU upload timing alone still cannot cover shader
                // guards/neutralization. Net cadence decides; unknown costs stay unknown.
                evidence.cpu_overhead_ms=cpu_overhead_ms;evidence.gpu_overhead_ms=std::numeric_limits<double>::quiet_NaN();
                evidence.gpu_execution_confirmed=submitted;evidence.evidence_age_frames=frame_number()-s.capture_evidence_frame;
                evidence.worst_tile=metric("worst_tile_linear_rgb_error");
                const auto temporal=judgement.value("temporal",Json());evidence.temporal_reference_matched=temporal.is_object()&&temporal.value("matched_reference",false);evidence.temporal_p99=temporal.is_object()?temporal.value("p99_tile_error",std::numeric_limits<double>::quiet_NaN()):std::numeric_limits<double>::quiet_NaN();
                if(const auto requested=s.requested_target.exchange(0);requested>0){s.target=requested;policy.target(requested);}
                ensure_surface();const auto decision=policy.evidence(evidence);
                if(!revalidation&&candidate_mode>=1&&(candidate_mode<=7||(candidate_mode>=11&&candidate_mode<=18))&&candidate.find(candidate_pipeline)->protect_edges&&judgement.value("matched_reference",false)){
                    auto& search=tuning[{candidate_pipeline,candidate_mode}];++search.rounds;const auto before=candidate.find(candidate_pipeline)->edge_threshold;
                    if(judgement.value("accepted_quality",false)){search.good=std::max(search.good,before);search.current=search.failed?(search.good+search.bad)*.5f:std::min(1.f,before*2);}
                    else{search.failed=true;search.bad=std::min(search.bad,before);search.current=(search.good+search.bad)*.5f;}
                    if(std::abs(search.current-before)<.02f)search.rounds=8;
                    journal({{"phase","spatial_search_updated"},{"pipeline",candidate_pipeline},{"mode",candidate_mode},{"tested_threshold",before},{"next_threshold",search.current},{"quality_pass",judgement.value("accepted_quality",false)},{"round",search.rounds}});
                }else if(!judgement.value("matched_reference",false))retry_after[candidate.id]=frame_number()+300;
                if(revalidation&&decision.kind!=SessionRequestKind::Apply&&judgement.value("matched_reference",false)&&!judgement.value("accepted_quality",false))verified={};
                if(decision.kind==SessionRequestKind::Apply){if(!candidate.compute.empty()&&!optimizer::seal_binding_evidence(candidate.id))throw TrialInterrupted("binding_evidence_not_sealable");s.retained_action=decision.action;s.retained_until=s.capture_evidence_frame+600;optimizer::approve_policy(decision.action,s.retained_until);select(candidate);incumbent=candidate;for(const auto& setting:candidate.compute)optimizer::remember_performance_proposal(setting,s.quality_profile);verified={candidate,choice_cost,choice_cpu,candidate_mode,candidate_pipeline,.05,true};retained=decision.action;policy.applied(retained,true);last=frame_number();rebuild_choices();}
                else if(decision.kind==SessionRequestKind::Restore){select(L"off");retained=0;incumbent={};s.retained_action=s.retained_until=0;optimizer::approve_policy(0,0);const bool restored=wait_restoration();policy.restored(restored);if(!restored)throw std::runtime_error("Changed pipeline retirement not confirmed");rebuild_choices();}

                last_probe_frames=std::clamp<UINT64>(frame_number()-probe_started,128,480);
                const auto state=policy.snapshot();Json report{{"phase","trial_finished"},{"trial",trial},{"action",request.action},{"submission_observed",submitted},{"quality_pass",judgement.value("accepted_quality",false)},{"baseline_frame_ms",baseline_ms},{"candidate_frame_ms",candidate_ms},{"accepted",state.accepted},{"rejected",state.rejected},{"retained",retained},{"cost_evidence_available",false},{"original_policy_retired",restored_original}};
                if(decision.kind==SessionRequestKind::Apply)best_images=dir;else last_rejected_images=dir;
                if(q.contains("worst_tile_linear_rgb_error")&&q["worst_tile_linear_rgb_error"].is_number()){const auto error=q["worst_tile_linear_rgb_error"].get<double>();if(error>worst_image_error){worst_image_error=error;worst_images=dir;}}
                report["revalidation"]=revalidation;report["pipeline_generation"]=generation;report["gpu_execution_confirmed"]=submitted;report["quality_reason"]=judgement.value("reason",std::string{});report["baseline_before_ms"]=baseline_before_ms;report["baseline_after_ms"]=baseline_after_ms;report["timing_reference_stable"]=timing_stable;
                report["cpu_cost_available"]=std::isfinite(cpu_overhead_ms);report["cpu_overhead_ms"]=std::isfinite(cpu_overhead_ms)?Json(cpu_overhead_ms):Json(nullptr);report["cpu_measured_intervals"]=cpu_frames;
                report["renewal_timing_overlapped"]=renewal_complete;report["quality_wall_ms"]=quality_wall_ms;report["timing_samples"]=timing_samples;report["timing_attempts"]=timing_attempts;report["original_frame_ms"]=original_ms;
                report["result"]=decision.kind==SessionRequestKind::Apply?"accepted":evidence.evidence_age_frames>=600?"invalidated":!submitted||!judgement.value("matched_reference",false)?"inconclusive":!judgement.value("accepted_quality",false)?"rejected_quality":!timing_stable?"inconclusive":"rejected_performance";
                report["captured_frame"]=s.capture_evidence_frame;report["proof_until_frame"]=s.capture_evidence_frame+600;report["binding_revision"]=s.transaction_bindings;report["surface_revision"]=s.transaction_revision;report["gpu_proof_path"]=(dir/L"submission-provenance.json").string();
                report["trial_wall_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-probe_wall_started).count();report["captured_changed_tile_fraction"]=s.captured_work_fraction;report["temporal"]=judgement.value("temporal",Json());report["quality_worker_launches"]=judgement.value("worker_launches",0u);report["evidence_age_frames"]=evidence.evidence_age_frames;report["quality_profile"]=s.quality_profile;
                report["gpu_calibration"]=gpu_calibration;report["quality"]=q;report["evidence_directory"]=dir.string();
                report["measured_noise_ms"]=std::isfinite(evidence.baseline_noise_ms)?Json(evidence.baseline_noise_ms):Json(nullptr);report["noise_method"]="max_A_drift_and_two_standard_errors";
                report["required_gain_ms"]=std::max({.1,baseline_ms*.02,evidence.baseline_noise_ms});
                report["decision_reason"]=decision.kind==SessionRequestKind::Apply?"accepted_net_gain_and_quality":!submitted?"gpu_execution_or_spatial_effect_unproven":!evidence.matched_reference?"reference_unmatched":!judgement.value("accepted_quality",false)?"quality_threshold_failed":!timing_stable?"timing_reference_unstable":evidence.evidence_age_frames>=600?"quality_evidence_expired":baseline_ms-candidate_ms<=report["required_gain_ms"].get<double>()?"gain_below_noise_or_threshold":"policy_evidence_rejected";
                report["bundle_targets"]=Json::array();for(const auto& p:candidate.compute)report["bundle_targets"].push_back({{"pipeline",p.pipeline},{"rate",{p.x_rate,p.y_rate}},{"pcf_taps",p.comparison_taps},{"zero_factor",p.zero_factor},{"mip_steps",p.mip_steps},{"sample_percent",p.sample_percent},{"protect_edges",p.protect_edges},{"edge_threshold",p.edge_threshold}});
                report["cpu_state_cache"]=candidate.cpu_state_cache;
                {std::ofstream file(dir/L"decision.json");file<<report.dump(2)<<'\n';}publish(std::move(report));rebuild_choices();
                }catch(const TrialInterrupted& error){select(L"off");optimizer::sample_frame_state(false);generic::cancel_image_sequence();const auto reset=policy.scene_changed();if(reset.kind==SessionRequestKind::Restore)policy.restored(wait_restoration());incumbent={};retained=0;s.retained_action=s.retained_until=0;optimizer::reset_binding_evidence();rebuild_choices();publish({{"phase","warmup"},{"result","invalidated"},{"trial",trial},{"reason",error.what()},{"surface_revision",s.surface_revision.load()}});}
            }else if(request.kind==SessionRequestKind::Restore){select(L"off");retained=0;incumbent={};s.retained_action=s.retained_until=0;optimizer::approve_policy(0,0);const bool restored=wait_restoration();policy.restored(restored);if(!restored)throw std::runtime_error("GPU policy retirement not confirmed");optimizer::reset_binding_evidence();rebuild_choices();}
        }
        select(L"off");const bool restored=wait_restoration(true);publish({{"phase",restored?"stopped":"faulted"},{"restored_request_sent",true},{"restoration_confirmed",restored}});
    }catch(const std::exception& error){optimizer::configure(L"off");const bool restored=wait_restoration(true);try{publish({{"phase",s.cancel&&restored?"stopped":"faulted"},{"reason",s.cancel?"cancelled_by_host":error.what()},{"restored_request_sent",true},{"restoration_confirmed",restored}});}catch(...) {}}
    s.quality_worker.reset();s.running=false;return 0;
}
}
bool start(const wchar_t* config_path)noexcept{
    if(!config_path||!optimizer::enabled()||mirror::requested_rate()||gpu_profile::busy()||!generic::gpu_helpers_idle())return false;auto& s=state();bool expected=false;if(!s.running.compare_exchange_strong(expected,true))return false;
    try{const auto config=read_json(config_path);s.quality_profile=config.value("quality_profile",std::string("balanced"));arc::optimizer_quality_limits(s.quality_profile);s.diagnostics_enabled=config.value("diagnostics_overlay",false);const auto control_mode=config.value("control_mode",std::string("validated"));if(control_mode!="validated"&&control_mode!="target_feedback")throw std::runtime_error("Unknown control mode");s.feedback_only=control_mode=="target_feedback";s.maximize=s.feedback_only?false:config.value("maximize_fps",true);s.target=config.at("target_fps");if(!std::isfinite(s.target)||s.target<=0||s.target>1000)throw std::runtime_error("Target FPS");
        s.python=std::filesystem::u8path(config.value("python",std::string{}));s.critic=std::filesystem::u8path(config.value("critic",std::string{}));s.directory=std::filesystem::u8path(config.at("output").get<std::string>());s.maximum_seconds=config.value("maximum_seconds",0u);
        if((!s.feedback_only&&(!s.python.is_absolute()||!s.critic.is_absolute()||!std::filesystem::is_regular_file(s.python)||!std::filesystem::is_regular_file(s.critic)))||!s.directory.is_absolute()||std::filesystem::exists(s.directory))throw std::runtime_error("Session paths");
        std::filesystem::create_directories(s.directory);{std::lock_guard lock(s.mutex);s.started=Clock::now();s.event_sequence=s.journal_failures=0;s.telemetry_device.Reset();s.swapchain=nullptr;s.window=nullptr;s.width=s.height=0;s.fullscreen=false;s.swapchain_identity=0;s.surface_revision=0;s.transaction_revision=0;s.retained_action=s.retained_until=0;s.capture_invalid=false;s.last_qpc=s.frames=0;s.count=s.cursor=0;s.cancel=false;s.requested_target=0;s.capture_active=false;s.capture_stage=0;s.capture_bundle={};LARGE_INTEGER f{};QueryPerformanceFrequency(&f);s.frequency=double(f.QuadPart);}
        const auto importance_profile=config.value("importance_profile",std::string("balanced"));if(importance_profile!="balanced"&&importance_profile!="center")throw std::runtime_error("Unknown importance profile");optimizer::center_priority(importance_profile=="center");
        optimizer::configure(L"off");optimizer::reset_binding_evidence();optimizer::reset_activity_counters();s.observing=true;HANDLE thread=CreateThread(nullptr,0,run,nullptr,0,nullptr);if(!thread)throw std::runtime_error("Session thread");CloseHandle(thread);return true;
    }catch(...){s.running=false;return false;}
}
void stop()noexcept{auto& s=state();std::lock_guard lock(s.policy_mutex);s.cancel=true;s.changed.notify_all();pixel::configure(0);mirror::configure(0);optimizer::approve_policy(0,0);optimizer::pause_spatial_probes(true);optimizer::configure(L"off");optimizer::cpu_configure(false);optimizer::sample_frame_state(false);placement::configure(L"normal");}
bool target(double fps)noexcept{auto& s=state();if(!std::isfinite(fps)||fps<=0||fps>1000||!s.running||s.cancel)return false;s.requested_target=fps;return true;}
bool active()noexcept{return state().running.load();}
void diagnostics(bool enabled)noexcept{state().diagnostics_enabled=enabled;}
void surface_changed(void* swap)noexcept{auto& s=state();if(!s.observing)return;std::lock_guard lock(s.mutex);if(s.swapchain==swap)invalidate_surface_locked();}
bool configure_runtime(const wchar_t* path)noexcept{
    if(!path||active())return false;
    try{const auto config=read_json(path);
        const wchar_t* names[]{L"ARC_OPTIMIZER_WORKER",L"ARC_OPTIMIZER_COMPILER",L"ARC_OPTIMIZER_CACHE"};
        const char* keys[]{"worker","compiler","cache"};std::array<std::filesystem::path,3> paths;
        for(unsigned i=0;i<paths.size();++i){paths[i]=std::filesystem::u8path(config.at(keys[i]).get<std::string>());if(!paths[i].is_absolute()||(i<2&&!std::filesystem::is_regular_file(paths[i])))return false;}
        // Validate the complete request before changing process-local settings.
        for(unsigned i=0;i<paths.size();++i)if(!SetEnvironmentVariableW(names[i],paths[i].c_str()))return false;
        return SetEnvironmentVariableW(L"ARC_AUTO_CONFIG",path)!=FALSE;
    }catch(...){return false;}
}
void present(void* swap,HRESULT result,UINT flags)noexcept{
    auto& s=state();if(!s.observing||result!=S_OK||(flags&DXGI_PRESENT_TEST))return;LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
    const auto identity=generic::presentation_identity(reinterpret_cast<IDXGISwapChain*>(swap));
    std::lock_guard lock(s.mutex);
    if(s.swapchain!=swap||s.swapchain_identity!=identity||!s.width){DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(reinterpret_cast<IDXGISwapChain*>(swap)->GetDesc(&desc)))return;
        if(s.swapchain&&s.swapchain!=swap&&desc.OutputWindow!=s.window&&s.last_qpc&&double(qpc.QuadPart-s.last_qpc)<s.frequency*.5)return;
        if(s.swapchain&&(s.swapchain!=swap||s.swapchain_identity!=identity))invalidate_surface_locked();
        const bool refresh_surface=s.swapchain!=swap||s.swapchain_identity!=identity||s.width!=desc.BufferDesc.Width||s.height!=desc.BufferDesc.Height||s.surface_format!=desc.BufferDesc.Format;s.swapchain=swap;s.swapchain_identity=identity;s.window=desc.OutputWindow;s.width=desc.BufferDesc.Width;s.height=desc.BufferDesc.Height;s.surface_format=desc.BufferDesc.Format;s.fullscreen=!desc.Windowed;if(refresh_surface){optimizer::presentation_surface(static_cast<IDXGISwapChain*>(swap));if(s.feedback_only)static_cast<IDXGISwapChain*>(swap)->GetDevice(IID_PPV_ARGS(s.telemetry_device.ReleaseAndGetAddressOf()));}
    }
    if(s.capture_active&&!s.cancel){const auto stage=generic::image_sequence_progress();
        if(stage!=s.capture_stage){
            if(stage>=1&&stage<=5){s.capture_stamps[stage-1]=optimizer::policy_stamp();try{s.frame_states[stage-1]=optimizer::frame_state_sample();}catch(...){s.cancel=true;}}
            // The next policy is selected at the Present boundary, before the
            // application's next frame. File IO runs independently on a worker.
            std::lock_guard policy_lock(s.policy_mutex);
            if(!s.cancel&&!optimizer::configure_bundle((stage==1||stage==3)?s.capture_bundle:arc::PolicyBundle{},true,0,s.frames+240))s.cancel=true;
            if(stage==1||stage==3)s.candidate_epochs[(stage-1)/2]=optimizer::policy_stamp().epoch;
            if(stage==2)s.capture_evidence_frame=s.frames;
            s.capture_stage=stage;if(stage>=5){s.capture_active=false;}
        }
    }
    if(s.last_qpc){const auto elapsed=(double(qpc.QuadPart)-double(s.last_qpc))*1000/s.frequency;if(elapsed>0&&std::isfinite(elapsed)){s.periods[s.cursor++%s.periods.size()]=elapsed;s.count=std::min<unsigned>(s.count+1,static_cast<unsigned>(s.periods.size()));}}
    s.last_qpc=qpc.QuadPart;++s.frames;optimizer::present_frame(s.frames,std::uint64_t(qpc.QuadPart));s.changed.notify_all();
}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);const auto& s=state();auto status=s.status;status["active_action"]=optimizer::active_approved_policy();status["present_samples"]=s.frames;status["objective"]=s.maximize?"maximize_fps":"target_fps";status["window_handle"]=reinterpret_cast<std::uintptr_t>(s.window);status["diagnostics_enabled"]=s.diagnostics_enabled.load();status["surface_revision"]=s.surface_revision.load();status["exclusive_fullscreen"]=s.fullscreen;
    const auto count=std::min(32u,s.count);if(count){double total=0;for(unsigned i=0;i<count;++i)total+=s.periods[(s.cursor+s.periods.size()-1-i)%s.periods.size()];if(total>0){status["current_fps"]=1000*count/total;status["current_frame_ms"]=total/count;}}
    out<<status.dump();}
}
