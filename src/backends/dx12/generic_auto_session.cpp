#include "generic_auto_session.hpp"
#include "generic_optimizer.hpp"
#include "generic_runtime.hpp"
#include "generic_gpu_profile.hpp"
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
    bool capture_active{};unsigned capture_stage{};std::wstring capture_mode;
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
bool wait_frames(unsigned count){auto& s=state();std::unique_lock lock(s.mutex);const auto target=s.frames+count;
    const bool woke=s.changed.wait_for(lock,std::chrono::seconds(4),[&]{return s.cancel||s.frames>=target;});return woke&&!s.cancel;}
double period(unsigned samples=32){std::lock_guard lock(state().mutex);const auto& s=state();const auto n=std::min(samples,s.count);if(!n)return 0;double sum=0;for(unsigned i=0;i<n;++i)sum+=s.periods[(s.cursor+s.periods.size()-1-i)%s.periods.size()];return sum/n;}
bool wait_file(const std::filesystem::path& file,unsigned milliseconds=3000){const auto until=Clock::now()+std::chrono::milliseconds(milliseconds);while(!state().cancel&&Clock::now()<until){if(std::filesystem::is_regular_file(file))return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;}
Json read_json(const std::filesystem::path& path){if(std::filesystem::file_size(path)>2*1024*1024)throw std::runtime_error("Quality JSON capacity");std::ifstream file(path);return Json::parse(file);}
Json quality(const std::filesystem::path& a,const std::filesystem::path& b,const std::filesystem::path& c,const std::filesystem::path& output){
    auto& s=state();auto command=quote(s.python.wstring())+L" "+quote(s.critic.wstring())+L" "+quote(a.wstring())+L" "+quote(b.wstring())+L" "+quote(c.wstring())+L" "+quote(output.wstring());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    // A host exit must not leave the critic consuming a CPU core in the
    // background. Assign the suspended process before allowing it to run.
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);throw std::runtime_error("Quality worker lifetime job");}
    if(!CreateProcessW(s.python.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,s.directory.c_str(),&startup,&process)){CloseHandle(job);throw std::runtime_error("Quality worker launch");}
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
bool capture_trial(const std::array<std::filesystem::path,3>& paths,const std::wstring& mode){
    auto& s=state();select(L"neutral|heaviest");if(!wait_frames(8))return false;
    {std::lock_guard lock(s.mutex);if(s.cancel)return false;s.capture_mode=mode;s.capture_stage=0;
        s.capture_active=generic::request_image_sequence(paths,reinterpret_cast<IDXGISwapChain*>(s.swapchain));
        if(!s.capture_active)return false;}
    bool complete=true;for(const auto& path:paths)complete=wait_file(path,4000)&&complete;
    {std::lock_guard lock(s.mutex);s.capture_active=false;}
    select(L"off");return complete;
}
DWORD WINAPI run(void*){
    auto& s=state();SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);const auto started=Clock::now();
    try{
        arc::OptimizerSessionConfig config;config.target_fps=s.target;config.warmup_samples=32;config.settle_samples=8;config.hold_samples=120;
        arc::OptimizerSession policy(config);
        const std::vector<std::wstring> modes={L"zero|heaviest",L"pcf9|heaviest",L"adaptive-1x2@0.5|heaviest",L"adaptive-2x2@0.5|heaviest",L"adaptive-2x2@0.75|heaviest",L"adaptive-2x2@0.9|heaviest",L"1x2|heaviest",L"2x2|heaviest"};
        std::vector<SessionAction> actions;for(unsigned i=0;i<modes.size();++i)actions.push_back({i+1,1,.2+double(i)*.2,.02+double(i)*.1,true});policy.candidates(std::move(actions));
        UINT64 last=0,trial=0,profile=0;std::uint64_t retained=0;
        publish({{"phase","warmup"},{"target_fps",s.target},{"quality_reference","live_motion_qualified"}});
        while(!s.cancel){
            if(s.maximum_seconds&&std::chrono::duration<double>(Clock::now()-started).count()>s.maximum_seconds)break;
            if(!wait_frames(1)){select(L"off");publish({{"phase","inactive"},{"reason","no_present_progress"}});continue;}
            const auto now=frame_number();if(now==last)continue;last=now;
            const auto request=policy.frame(period());
            if(request.kind==SessionRequestKind::Profile){
                select(L"neutral|heaviest");const auto path=s.directory/(L"profile-"+std::to_wstring(++profile)+L".json");
                if(!gpu_profile::request(path.wstring(),16)||!wait_file(path,12000))throw std::runtime_error("GPU discovery unavailable");
                publish({{"phase","discovery_complete"},{"target_fps",s.target}});
            }else if(request.kind==SessionRequestKind::Probe){
                const auto dir=s.directory/(L"trial-"+std::to_wstring(++trial));std::filesystem::create_directory(dir);
                const auto mode=modes.at(request.action-1);select(retained?modes.at(retained-1):L"off");if(!wait_frames(40))break;const auto baseline_ms=period();
                // The final-image reference always uses the ORIGINAL policy,
                // even when comparing a replacement against a retained setting.
                const auto a=dir/L"before.json",b=dir/L"candidate.json",c=dir/L"after.json";
                if(!capture_trial({a,b,c},mode))throw std::runtime_error("Consecutive reference capture unavailable");
                auto judgement=quality(a,b,c,dir/L"quality.json");
                if(s.cancel)break;
                double candidate_ms=baseline_ms;
                if(judgement.value("accepted_quality",false)){select(mode);if(!wait_frames(48))break;candidate_ms=period();}
                select(retained?modes.at(retained-1):L"off");if(!wait_frames(8))break;
                const auto& q=judgement.at("quality");OptimizerTrialEvidence evidence;
                evidence.action=request.action;evidence.generation=1;evidence.complete=true;evidence.restoration_confirmed=true;evidence.matched_reference=judgement.value("matched_reference",false);
                evidence.baseline_frame_ms=baseline_ms;evidence.candidate_frame_ms=candidate_ms;evidence.baseline_noise_ms=baseline_ms*.03;
                evidence.ssim=q.at("ssim_gaussian_luma");evidence.mean_error=q.at("mean_linear_rgb_error");evidence.tile_p99=q.at("p99_tile_linear_rgb_error");
                // Until own CPU/GPU overhead is measured, the evidence must not
                // pass the cost gate. A fast-looking cadence is insufficient.
                evidence.cpu_overhead_ms=std::numeric_limits<double>::infinity();evidence.gpu_overhead_ms=std::numeric_limits<double>::infinity();
                const auto decision=policy.evidence(evidence);
                if(decision.kind==SessionRequestKind::Apply){select(mode);retained=decision.action;policy.applied(retained,true);}
                const auto state=policy.snapshot();publish({{"phase","trial_finished"},{"trial",trial},{"action",request.action},{"quality_pass",judgement.value("accepted_quality",false)},{"baseline_frame_ms",baseline_ms},{"candidate_frame_ms",candidate_ms},{"accepted",state.accepted},{"rejected",state.rejected},{"retained",retained},{"cost_evidence_available",false}});
            }else if(request.kind==SessionRequestKind::Restore){select(L"off");retained=0;policy.restored(true);}
        }
        select(L"off");publish({{"phase","stopped"},{"restored_request_sent",true}});
    }catch(const std::exception& error){optimizer::configure(L"off");try{publish({{"phase",s.cancel?"stopped":"faulted"},{"reason",error.what()},{"restored_request_sent",true}});}catch(...) {}}
    s.running=false;return 0;
}
}
bool start(const wchar_t* config_path)noexcept{
    if(!config_path||!optimizer::enabled())return false;auto& s=state();bool expected=false;if(!s.running.compare_exchange_strong(expected,true))return false;
    try{const auto config=read_json(config_path);s.target=config.at("target_fps");if(!std::isfinite(s.target)||s.target<=0||s.target>1000)throw std::runtime_error("Target FPS");
        s.python=config.at("python").get<std::string>();s.critic=config.at("critic").get<std::string>();s.directory=config.at("output").get<std::string>();s.maximum_seconds=config.value("maximum_seconds",0u);
        if(!s.python.is_absolute()||!s.critic.is_absolute()||!s.directory.is_absolute()||!std::filesystem::is_regular_file(s.python)||!std::filesystem::is_regular_file(s.critic)||std::filesystem::exists(s.directory))throw std::runtime_error("Session paths");
        std::filesystem::create_directories(s.directory);{std::lock_guard lock(s.mutex);s.swapchain=nullptr;s.last_qpc=s.frames=0;s.count=s.cursor=0;s.cancel=false;s.capture_active=false;s.capture_stage=0;s.capture_mode.clear();LARGE_INTEGER f{};QueryPerformanceFrequency(&f);s.frequency=double(f.QuadPart);}
        HANDLE thread=CreateThread(nullptr,0,run,nullptr,0,nullptr);if(!thread)throw std::runtime_error("Session thread");CloseHandle(thread);return true;
    }catch(...){s.running=false;return false;}
}
void stop()noexcept{auto& s=state();std::lock_guard lock(s.policy_mutex);s.cancel=true;s.changed.notify_all();optimizer::configure(L"off");}
void present(void* swap,HRESULT result,UINT flags)noexcept{
    auto& s=state();if(!s.running||result!=S_OK||(flags&DXGI_PRESENT_TEST))return;LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
    std::lock_guard lock(s.mutex);if(s.swapchain&&s.swapchain!=swap)return;s.swapchain=swap;
    if(s.capture_active&&!s.cancel){const auto stage=generic::image_sequence_progress();
        if(stage!=s.capture_stage){
            // The next policy is selected at the Present boundary, before the
            // application's next frame. File IO runs independently on a worker.
            std::lock_guard policy_lock(s.policy_mutex);
            const auto* mode=stage==1?s.capture_mode.c_str():L"neutral|heaviest";
            if(!s.cancel&&!optimizer::configure(mode))s.cancel=true;
            s.capture_stage=stage;if(stage>=3)s.capture_active=false;
        }
    }
    if(s.last_qpc){const auto elapsed=(double(qpc.QuadPart)-double(s.last_qpc))*1000/s.frequency;if(elapsed>0&&elapsed<1000){s.periods[s.cursor++%s.periods.size()]=elapsed;s.count=std::min<unsigned>(s.count+1,static_cast<unsigned>(s.periods.size()));}}
    s.last_qpc=qpc.QuadPart;++s.frames;s.changed.notify_all();
}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);out<<state().status.dump();}
}
