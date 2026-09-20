#include "generic_auto_session.hpp"
#include "generic_optimizer.hpp"
#include "generic_runtime.hpp"
#include "generic_gpu_profile.hpp"
#include "generic_command_mirror.hpp"
#include "generic_cpu_workers.hpp"
#include "arc/intercept_cpu_meter.hpp"
#include "arc/optimizer_session.hpp"
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

namespace arc::dx12::autotune {
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct State {
    std::atomic<bool> running{},cancel{};
    std::mutex mutex,policy_mutex;std::condition_variable changed;
    void* swapchain{};UINT64 last_qpc{},frames{};double frequency{};
    std::array<double,128> periods{};unsigned count{},cursor{};
    Json status{{"phase","off"}};
    std::filesystem::path directory,python,critic;
    double target{};unsigned maximum_seconds{0};
    bool capture_active{};unsigned capture_stage{};arc::PolicyBundle capture_bundle;
    std::array<optimizer::PolicyStamp,3> capture_stamps{};std::uint64_t candidate_epoch{};
    std::array<optimizer::FrameStateSample,3> frame_states;
};
State& state(){static auto* s=new State;return *s;}
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned n=0;for(auto c:s){if(c==L'\\'){++n;continue;}if(c==L'"'){out.append(n*2+1,L'\\');out+=c;}else{out.append(n,L'\\');out+=c;}n=0;}out.append(n*2,L'\\');return out+L'"';}
void publish(Json value){
    auto& s=state();{std::lock_guard lock(s.mutex);s.status=value;}
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
    const bool woke=s.changed.wait_for(lock,std::chrono::seconds(4),[&]{return s.cancel||s.frames>=target;});return woke&&!s.cancel;}
double period(unsigned samples=32){std::lock_guard lock(state().mutex);const auto& s=state();const auto n=std::min(samples,s.count);if(!n)return 0;double sum=0;for(unsigned i=0;i<n;++i)sum+=s.periods[(s.cursor+s.periods.size()-1-i)%s.periods.size()];return sum/n;}
bool wait_file(const std::filesystem::path& file,unsigned milliseconds=3000){const auto until=Clock::now()+std::chrono::milliseconds(milliseconds);while(!state().cancel&&Clock::now()<until){if(std::filesystem::is_regular_file(file))return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;}
bool wait_restoration(bool finishing=false){const auto until=Clock::now()+std::chrono::seconds(3);while((finishing||!state().cancel)&&Clock::now()<until){if(optimizer::restoration_ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;}
Json read_json(const std::filesystem::path& path){if(std::filesystem::file_size(path)>2*1024*1024)throw std::runtime_error("Quality JSON capacity");std::ifstream file(path);return Json::parse(file);}
Json quality(const std::filesystem::path& a,const std::filesystem::path& b,const std::filesystem::path& c,const std::filesystem::path& output){
    auto& s=state();auto command=quote(s.python.wstring())+L" "+quote(s.critic.wstring())+L" "+quote(a.wstring())+L" "+quote(b.wstring())+L" "+quote(c.wstring())+L" "+quote(output.wstring())+L" --state "+quote((output.parent_path()/L"frame-state.json").wstring());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    // A host exit must not leave the critic consuming a CPU core in the
    // background. Assign the suspended process before allowing it to run.
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);throw std::runtime_error("Quality worker lifetime job");}
    if(!CreateProcessW(s.python.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,s.directory.c_str(),&startup,&process)){CloseHandle(job);throw std::runtime_error("Quality worker launch");}
    cpu_cost::Registration child_cpu(cpu_cost::Kind::Critic,process.hProcess,process.hThread);
    if(!AssignProcessToJobObject(job,process.hProcess)||ResumeThread(process.hThread)==DWORD(-1)){
        TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);throw std::runtime_error("Quality worker lifetime assignment");}
    CloseHandle(process.hThread);DWORD code=1,waited=WAIT_TIMEOUT;const auto deadline=Clock::now()+std::chrono::seconds(15);
    while(!s.cancel&&Clock::now()<deadline&&(waited=WaitForSingleObject(process.hProcess,50))==WAIT_TIMEOUT){}
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else{TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);}CloseHandle(process.hProcess);CloseHandle(job);
    if(code||!std::filesystem::is_regular_file(output))throw std::runtime_error("Quality worker failed");return read_json(output);
}
void select(const std::wstring& mode){auto& s=state();std::lock_guard lock(s.policy_mutex);
    if(s.cancel&&mode!=L"off")throw std::runtime_error("Session cancelled");
    if(!optimizer::configure(mode.c_str()))throw std::runtime_error("Optimizer policy switch refused");}
void select(const arc::PolicyBundle& bundle){auto& s=state();std::lock_guard lock(s.policy_mutex);
    if(s.cancel&&!bundle.compute.empty())throw std::runtime_error("Session cancelled");
    if(!optimizer::configure_bundle(bundle))throw std::runtime_error("Optimizer bundle refused");}
bool capture_trial(const std::array<std::filesystem::path,3>& paths,const arc::PolicyBundle& bundle,bool& observed_submission){
    struct EndSampling {~EndSampling(){optimizer::sample_frame_state(false);}} end_sampling;
    auto& s=state();select(arc::PolicyBundle{});if(!wait_frames(8))return false;
    optimizer::sample_frame_state(true);if(!wait_frames(2)){optimizer::sample_frame_state(false);return false;}
    {std::lock_guard lock(s.mutex);if(s.cancel)return false;s.capture_bundle=bundle;s.capture_stage=0;s.capture_stamps={};s.frame_states={};s.candidate_epoch=0;
        s.capture_active=generic::request_image_sequence(paths,reinterpret_cast<IDXGISwapChain*>(s.swapchain));
        if(!s.capture_active){optimizer::sample_frame_state(false);return false;}}
    bool complete=true;for(const auto& path:paths)complete=wait_file(path,4000)&&complete;
    Json provenance,state_samples=Json::array();
    {std::lock_guard lock(s.mutex);s.capture_active=false;const auto& a=s.capture_stamps[0];const auto& b=s.capture_stamps[1];const auto& c=s.capture_stamps[2];
        observed_submission=s.candidate_epoch&&b.epoch==s.candidate_epoch&&b.last_active_epoch==s.candidate_epoch&&b.active_submissions>a.active_submissions&&c.active_submissions==b.active_submissions;
        provenance={{"kind","cpu_submission_epoch"},{"candidate_epoch",s.candidate_epoch},{"submission_observed",observed_submission},{"same_frame_gpu_dependency_proven",false},{"active_submissions",{a.active_submissions,b.active_submissions,c.active_submissions}}};
        for(const auto& sample:s.frame_states)state_samples.push_back({{"pipeline",sample.pipeline},{"submission",sample.submission},{"keys",sample.keys},{"words",sample.words},{"valid",sample.valid}});}
    optimizer::sample_frame_state(false);
    {std::ofstream file(paths[0].parent_path()/L"submission-provenance.json");file<<provenance.dump(2)<<'\n';}
    {std::ofstream file(paths[0].parent_path()/L"frame-state.json");file<<state_samples.dump(2)<<'\n';}
    select(L"off");return complete;
}
DWORD WINAPI run(void*){
    cpu_cost::Registration worker_cpu;
    arc::InterceptCpuMeter::enable(true);
    auto& s=state();SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);const auto started=Clock::now();
    try{
        arc::OptimizerSessionConfig config;config.target_fps=s.target;config.warmup_samples=32;config.settle_samples=8;config.hold_samples=120;
        arc::OptimizerSession policy(config);
        struct Choice {arc::PolicyBundle bundle;double cost{};};
        std::vector<Choice> choices;arc::PolicyBundle incumbent;std::uint64_t next_choice=0;
        std::vector<optimizer::WorkCandidate> catalog;
        auto refresh_candidates=[&]{
            std::vector<SessionAction> actions;
            for(const auto& choice:choices)actions.push_back({choice.bundle.id,choice.bundle.id,0,1,true,choice.cost,false});
            if(incumbent.id)actions.push_back({incumbent.id,incumbent.id,0,1,true,0,false});
            policy.candidates(std::move(actions));
        };
        auto rebuild_choices=[&]{
            choices.clear();
            for(const auto& work:catalog){const auto& c=work.capabilities;
                for(unsigned mode=0;mode<11;++mode){
                    const bool ready[]{c.zero,c.comparison,c.coarse&&c.edges,c.coarse&&c.edges,c.coarse&&c.edges,c.coarse&&c.edges,c.coarse,c.coarse,c.mips,c.mips,c.mips};
                    if(!ready[mode])continue;
                    arc::ComputePolicy setting;setting.pipeline=c.pipeline;
                    if(mode==0)setting.zero_factor=1;
                    else if(mode==1)setting.comparison_taps=9;
                    else if(mode<=7){setting.x_rate=(mode==2||mode==6)?1:2;setting.y_rate=2;setting.protect_edges=mode<=5;setting.edge_threshold=mode<=3?.5f:mode==4?.75f:.9f;}
                    else setting.mip_steps=mode==8?1:mode==9?2:4;
                    if(const auto* current=incumbent.find(c.pipeline);current&&*current==setting)continue;
                    auto bundle=incumbent;bundle.id=++next_choice;
                    if(bundle.replace(setting))choices.push_back({std::move(bundle),work.gpu_ms_per_window});
                }
            }
            refresh_candidates();
        };
        refresh_candidates();
        UINT64 last=0,trial=0,profile=0,catalog_check=0;std::uint64_t retained=0;
        std::string idle_phase;
        publish({{"phase","warmup"},{"target_fps",s.target},{"quality_reference","live_motion_qualified"}});
        while(!s.cancel){
            if(s.maximum_seconds&&std::chrono::duration<double>(Clock::now()-started).count()>s.maximum_seconds)break;
            if(!wait_frames(1)){if(s.cancel)break;idle_phase.clear();select(L"off");publish({{"phase","inactive"},{"reason","no_present_progress"}});continue;}
            const auto now=frame_number();if(now==last)continue;last=now;
            // Compilation is asynchronous: a profile can finish before its
            // expensive pipeline is ready. Refresh bounded discovery without
            // inventing new action IDs on every frame or repeating rejections.
            if(profile&&now-catalog_check>=120){
                catalog_check=now;auto next=optimizer::candidate_catalog();
                const bool same=next.size()==catalog.size()&&std::equal(next.begin(),next.end(),catalog.begin(),[](const auto& a,const auto& b){return a.capabilities.pipeline==b.capabilities.pipeline&&a.profile_session==b.profile_session;});
                if(!same){catalog=std::move(next);rebuild_choices();}
            }
            const auto request=policy.frame(period());
            if(request.kind==SessionRequestKind::None){const auto current=policy.snapshot();
                if(current.phase==SessionPhase::Active||current.phase==SessionPhase::Limited){
                    const std::string phase=current.phase==SessionPhase::Limited?"limited":current.filtered_frame_ms<=current.target_frame_ms?"target_met":"holding";
                    if(phase!=idle_phase){idle_phase=phase;publish({{"phase",phase},{"target_fps",s.target},{"filtered_frame_ms",current.filtered_frame_ms},{"active_action",current.active_action},{"accepted",current.accepted},{"rejected",current.rejected}});}
                }
            }else idle_phase.clear();
            if(request.kind==SessionRequestKind::Profile){
                select(L"off");const auto path=s.directory/(L"profile-"+std::to_wstring(++profile)+L".json");
                if(!gpu_profile::request(path.wstring(),16)||!wait_file(path,12000))throw std::runtime_error("GPU discovery unavailable");
                catalog=optimizer::candidate_catalog();rebuild_choices();
                publish({{"phase","discovery_complete"},{"target_fps",s.target},{"measured_work_candidates",catalog.size()},{"policy_candidates",choices.size()}});
            }else if(request.kind==SessionRequestKind::Probe){
                const auto dir=s.directory/(L"trial-"+std::to_wstring(++trial));std::filesystem::create_directory(dir);
                const auto choice=std::find_if(choices.begin(),choices.end(),[&](const auto& c){return c.bundle.id==request.action;});
                if(choice==choices.end())throw std::runtime_error("Unknown bundle request");
                const auto candidate=choice->bundle;const auto generation=candidate.id;
                if(retained)select(incumbent);else select(L"off");if(!wait_frames(40))break;double baseline_ms=period();
                // The final-image reference always uses the ORIGINAL policy,
                // even when comparing a replacement against a retained setting.
                const auto a=dir/L"before.json",b=dir/L"candidate.json",c=dir/L"after.json";
                bool submitted=false;if(!capture_trial({a,b,c},candidate,submitted))throw std::runtime_error("Consecutive reference capture unavailable");
                // No observed active submission cannot establish an effect.
                // Avoid spending a CPU-heavy image trial on unchanged work.
                auto judgement=submitted?quality(a,b,c,dir/L"quality.json"):Json{{"accepted_quality",false},{"matched_reference",false},{"reason","no_policy_submission"},
                    {"quality",{{"ssim_gaussian_luma",0.0},{"mean_linear_rgb_error",1.0},{"p99_tile_linear_rgb_error",1.0}}}};
                if(s.cancel)break;
                double candidate_ms=baseline_ms,baseline_before_ms=baseline_ms,baseline_after_ms=baseline_ms;bool timing_stable=false;
                double cpu_overhead_ms=std::numeric_limits<double>::infinity();UINT64 cpu_frames=0;
                if(submitted&&judgement.value("accepted_quality",false)){
                    // Image analysis can take seconds. Measure cadence only
                    // afterwards, with incumbent windows on BOTH sides of B.
                    // A scene transition must not masquerade as a speedup.
                    if(retained)select(incumbent);else select(L"off");if(!wait_frames(40))break;baseline_before_ms=period();
                    select(candidate);if(!wait_frames(16))break;const auto cost_before=cpu_sample();
                    if(!wait_frames(32))break;const auto cost_after=cpu_sample();candidate_ms=period();
                    cpu_overhead_ms=cpu_window_ms(cost_before,cost_after);cpu_frames=cost_after.frame-cost_before.frame;
                    select(L"off");if(!wait_restoration())throw std::runtime_error("Timing policy retirement not confirmed");
                    if(retained)select(incumbent);else select(L"off");if(!wait_frames(40))break;baseline_after_ms=period();
                    baseline_ms=(baseline_before_ms+baseline_after_ms)*.5;
                    timing_stable=std::isfinite(baseline_ms)&&baseline_ms>0&&std::abs(baseline_before_ms-baseline_after_ms)<=baseline_ms*.1;
                }
                select(L"off");if(!wait_restoration())throw std::runtime_error("GPU policy retirement not confirmed");
                const bool restored_original=true;
                if(retained)select(incumbent);else select(L"off");if(!wait_frames(8))break;
                const auto& q=judgement.at("quality");OptimizerTrialEvidence evidence;
                evidence.action=request.action;evidence.generation=generation;evidence.complete=submitted&&timing_stable;evidence.restoration_confirmed=restored_original;evidence.matched_reference=submitted&&judgement.value("matched_reference",false);
                evidence.baseline_frame_ms=baseline_ms;evidence.candidate_frame_ms=candidate_ms;evidence.baseline_noise_ms=std::max(baseline_ms*.03,std::abs(baseline_before_ms-baseline_after_ms));
                evidence.ssim=q.at("ssim_gaussian_luma");evidence.mean_error=q.at("mean_linear_rgb_error");evidence.tile_p99=q.at("p99_tile_linear_rgb_error");
                // CPU covers hooks, lifetime callbacks, owned threads and live
                // children. GPU upload timing alone still cannot cover shader
                // guards/neutralization, so the combined cost gate stays shut.
                evidence.cpu_overhead_ms=cpu_overhead_ms;evidence.gpu_overhead_ms=std::numeric_limits<double>::infinity();
                const auto decision=policy.evidence(evidence);
                if(decision.kind==SessionRequestKind::Apply){select(candidate);incumbent=candidate;retained=decision.action;policy.applied(retained,true);rebuild_choices();}
                else if(decision.kind==SessionRequestKind::Restore){select(L"off");retained=0;incumbent={};const bool restored=wait_restoration();policy.restored(restored);if(!restored)throw std::runtime_error("Changed pipeline retirement not confirmed");}
                const auto state=policy.snapshot();Json report{{"phase","trial_finished"},{"trial",trial},{"action",request.action},{"submission_observed",submitted},{"quality_pass",judgement.value("accepted_quality",false)},{"baseline_frame_ms",baseline_ms},{"candidate_frame_ms",candidate_ms},{"accepted",state.accepted},{"rejected",state.rejected},{"retained",retained},{"cost_evidence_available",false},{"original_policy_retired",restored_original}};
                report["pipeline_generation"]=generation;report["baseline_before_ms"]=baseline_before_ms;report["baseline_after_ms"]=baseline_after_ms;report["timing_reference_stable"]=timing_stable;
                report["cpu_cost_available"]=std::isfinite(cpu_overhead_ms);report["cpu_overhead_ms"]=std::isfinite(cpu_overhead_ms)?Json(cpu_overhead_ms):Json(nullptr);report["cpu_measured_intervals"]=cpu_frames;
                report["bundle_targets"]=Json::array();for(const auto& p:candidate.compute)report["bundle_targets"].push_back({{"pipeline",p.pipeline},{"rate",{p.x_rate,p.y_rate}},{"pcf_taps",p.comparison_taps},{"zero_factor",p.zero_factor},{"mip_steps",p.mip_steps},{"protect_edges",p.protect_edges},{"edge_threshold",p.edge_threshold}});
                {std::ofstream file(dir/L"decision.json");file<<report.dump(2)<<'\n';}publish(std::move(report));
            }else if(request.kind==SessionRequestKind::Restore){select(L"off");retained=0;incumbent={};const bool restored=wait_restoration();policy.restored(restored);if(!restored)throw std::runtime_error("GPU policy retirement not confirmed");}
        }
        select(L"off");const bool restored=wait_restoration(true);publish({{"phase",restored?"stopped":"faulted"},{"restored_request_sent",true},{"restoration_confirmed",restored}});
    }catch(const std::exception& error){optimizer::configure(L"off");const bool restored=wait_restoration(true);try{publish({{"phase",s.cancel&&restored?"stopped":"faulted"},{"reason",s.cancel?"cancelled_by_host":error.what()},{"restored_request_sent",true},{"restoration_confirmed",restored}});}catch(...) {}}
    s.running=false;return 0;
}
}
bool start(const wchar_t* config_path)noexcept{
    if(!config_path||!optimizer::enabled()||mirror::requested_rate())return false;auto& s=state();bool expected=false;if(!s.running.compare_exchange_strong(expected,true))return false;
    try{const auto config=read_json(config_path);s.target=config.at("target_fps");if(!std::isfinite(s.target)||s.target<=0||s.target>1000)throw std::runtime_error("Target FPS");
        s.python=config.at("python").get<std::string>();s.critic=config.at("critic").get<std::string>();s.directory=config.at("output").get<std::string>();s.maximum_seconds=config.value("maximum_seconds",0u);
        if(!s.python.is_absolute()||!s.critic.is_absolute()||!s.directory.is_absolute()||!std::filesystem::is_regular_file(s.python)||!std::filesystem::is_regular_file(s.critic)||std::filesystem::exists(s.directory))throw std::runtime_error("Session paths");
        std::filesystem::create_directories(s.directory);{std::lock_guard lock(s.mutex);s.swapchain=nullptr;s.last_qpc=s.frames=0;s.count=s.cursor=0;s.cancel=false;s.capture_active=false;s.capture_stage=0;s.capture_bundle={};LARGE_INTEGER f{};QueryPerformanceFrequency(&f);s.frequency=double(f.QuadPart);}
        HANDLE thread=CreateThread(nullptr,0,run,nullptr,0,nullptr);if(!thread)throw std::runtime_error("Session thread");CloseHandle(thread);return true;
    }catch(...){s.running=false;return false;}
}
void stop()noexcept{auto& s=state();std::lock_guard lock(s.policy_mutex);s.cancel=true;s.changed.notify_all();optimizer::configure(L"off");optimizer::sample_frame_state(false);placement::configure(L"normal");}
void present(void* swap,HRESULT result,UINT flags)noexcept{
    auto& s=state();if(!s.running||result!=S_OK||(flags&DXGI_PRESENT_TEST))return;LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
    std::lock_guard lock(s.mutex);if(s.swapchain&&s.swapchain!=swap)return;s.swapchain=swap;
    if(s.capture_active&&!s.cancel){const auto stage=generic::image_sequence_progress();
        if(stage!=s.capture_stage){
            if(stage>=1&&stage<=3){s.capture_stamps[stage-1]=optimizer::policy_stamp();try{s.frame_states[stage-1]=optimizer::frame_state_sample();}catch(...){s.cancel=true;}}
            // The next policy is selected at the Present boundary, before the
            // application's next frame. File IO runs independently on a worker.
            std::lock_guard policy_lock(s.policy_mutex);
            if(!s.cancel&&!optimizer::configure_bundle(stage==1?s.capture_bundle:arc::PolicyBundle{}))s.cancel=true;
            if(stage==1)s.candidate_epoch=optimizer::policy_stamp().epoch;
            s.capture_stage=stage;if(stage>=3)s.capture_active=false;
        }
    }
    if(s.last_qpc){const auto elapsed=(double(qpc.QuadPart)-double(s.last_qpc))*1000/s.frequency;if(elapsed>0&&elapsed<1000){s.periods[s.cursor++%s.periods.size()]=elapsed;s.count=std::min<unsigned>(s.count+1,static_cast<unsigned>(s.periods.size()));}}
    s.last_qpc=qpc.QuadPart;++s.frames;s.changed.notify_all();
}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);out<<state().status.dump();}
}
