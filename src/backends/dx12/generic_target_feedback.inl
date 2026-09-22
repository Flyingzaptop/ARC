// Included inside autotune's private namespace. No image capture or critic.
Json feedback_memory(){
    PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);
    const bool ok=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))!=FALSE;
    Json result{{"process_private_bytes",ok?Json(m.PrivateUsage):Json(nullptr)},{"process_working_set_bytes",ok?Json(m.WorkingSetSize):Json(nullptr)},
        {"cpu_frame_ms",nullptr},{"gpu_frame_ms",nullptr},{"frame_timing_reason","whole_game_cpu_and_gpu_critical_path_not_instrumented"},
        {"vram_local_usage_bytes",nullptr},{"vram_local_budget_bytes",nullptr},{"memory_scope","game_process_including_arc"}};
    MEMORYSTATUSEX physical{};physical.dwLength=sizeof(physical);if(GlobalMemoryStatusEx(&physical)){result["system_ram_available_bytes"]=physical.ullAvailPhys;result["system_ram_total_bytes"]=physical.ullTotalPhys;}
    ID3D12Device* device{};
    {std::lock_guard lock(state().mutex);device=state().telemetry_device.Get();if(device)device->AddRef();}
    if(device){IDXGIFactory4* factory{};IDXGIAdapter3* adapter{};
        if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){
            if(SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))){
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};if(SUCCEEDED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info))){result["vram_local_usage_bytes"]=info.CurrentUsage;result["vram_local_budget_bytes"]=info.Budget;}
                adapter->Release();}factory->Release();}device->Release();}
    return result;
}
void run_target_feedback(){
    auto& s=state();optimizer::spatial_learning(false,0);optimizer::pause_spatial_probes(true);optimizer::sample_frame_state(false);
    arc::PolicyBundle current,previous;std::vector<arc::PolicyBundle> history;
    std::uint64_t sequence=0,profile=0,revision=s.surface_revision.load(),bindings=optimizer::binding_evidence_revision();
    std::map<std::pair<std::uint64_t,unsigned>,Clock::time_point> retry;
    std::vector<optimizer::WorkCandidate> catalog;
    auto last_profile=Clock::now()-std::chrono::seconds(30),last_decision=Clock::now();
    const auto start=Clock::now();bool pending=false,recovering=false;double before=0;arc::TargetFeedbackGate gate;
    std::pair<std::uint64_t,unsigned> pending_key{};std::uint64_t submitted_before{};
    auto apply=[&](const arc::PolicyBundle& bundle){
        std::lock_guard lock(s.policy_mutex);if(s.cancel)return false;
        // Zero expiry means no quality lease. Submission-time resource, bounds,
        // aliasing, queue and shader-contract checks remain in optimizer::execute.
        submitted_before=optimizer::policy_stamp().active_submissions;
        if(!optimizer::configure_bundle(bundle,true,0,0))return false;
        optimizer::approve_policy(bundle.id,std::numeric_limits<std::uint64_t>::max());return true;
    };
    auto report=[&](const char* phase,const char* reason){auto row=feedback_memory();row["phase"]=phase;row["reason"]=reason;row["target_fps"]=s.target;row["frame_ms"]=period();row["active_action"]=current.id;
        row["quality_verified"]=false;row["bottleneck"]="unknown_whole_frame_cpu_gpu_timing_unavailable";row["memory_pressure"]=row["vram_local_budget_bytes"].is_number()&&row["vram_local_budget_bytes"].get<double>()>0?Json(row["vram_local_usage_bytes"].get<double>()>row["vram_local_budget_bytes"].get<double>()*.95):Json(nullptr);row["performance_evidence"]= "sequential_observation_not_causal_ABA";row["targets"]=Json::array();
        for(const auto& p:current.compute)row["targets"].push_back({{"pipeline",p.pipeline},{"rate",{p.x_rate,p.y_rate}},{"mip_steps",p.mip_steps},{"sample_percent",p.sample_percent}});
        double cost=0;for(const auto& c:catalog)cost+=c.gpu_ms_per_window;row["profiled_supported_compute_ms"]=catalog.empty()?Json(nullptr):Json(cost);row["profile_age_seconds"]=std::chrono::duration<double>(Clock::now()-last_profile).count();publish(std::move(row));};
    report("warmup","target_feedback_no_image_critic");
    while(!s.cancel){
        if(s.maximum_seconds&&Clock::now()-start>std::chrono::seconds(s.maximum_seconds))break;
        if(!wait_frames(8)){if(s.cancel)break;select(L"off");optimizer::approve_policy(0,0);current={};history.clear();pending=false;gate.reset();report("inactive","no_present_progress");continue;}
        if(auto requested=s.requested_target.exchange(0);requested>0){s.target=requested;gate.reset();}
        const auto now=Clock::now();
        if(revision!=s.surface_revision.load()||bindings!=optimizer::binding_evidence_revision()){
            select(L"off");optimizer::approve_policy(0,0);current={};history.clear();pending=false;gate.reset();retry.clear();catalog.clear();
            optimizer::reset_binding_evidence();revision=s.surface_revision.load();bindings=optimizer::binding_evidence_revision();last_profile=now-std::chrono::seconds(30);report("warmup","technical_context_changed");
        }
        if(now-last_decision<std::chrono::seconds(2))continue;last_decision=now;
        if(now-last_profile>=std::chrono::seconds(15)&&!pending){
            const auto path=s.directory/(L"feedback-profile-"+std::to_wstring(++profile)+L".json");
            // Bounded rotating captures; profiling never disables the current policy.
            if(profile>3){std::error_code ec;std::filesystem::remove(s.directory/(L"feedback-profile-"+std::to_wstring(profile-3)+L".json"),ec);}
            if(gpu_profile::request(path.wstring(),16)&&wait_file(path,12000))catalog=optimizer::candidate_catalog();else gpu_profile::stop();
            last_profile=Clock::now();continue;
        }
        catalog=optimizer::candidate_catalog();
        std::stable_sort(catalog.begin(),catalog.end(),[](const auto& a,const auto& b){return a.gpu_ms_per_window>b.gpu_ms_per_window;});
        const double ft=period(),budget=1000./s.target;if(!std::isfinite(ft)||ft<=0)continue;
        if(pending){
            // A sequential comparison can be confounded by scene changes. Keep
            // that uncertainty explicit instead of fabricating quality evidence.
            const bool no_submission=!current.compute.empty()&&optimizer::policy_stamp().active_submissions<=submitted_before;
            if(no_submission||arc::reject_feedback_change(before,ft,budget,recovering)){if(!apply(previous))throw TrialInterrupted("feedback_restore_refused");current=previous;retry[pending_key]=now+std::chrono::seconds(30);report("holding",no_submission?"no_modified_submission_reverted":"sequential_regression_reverted");}
            else{if(recovering){if(!history.empty())history.pop_back();}else history.push_back(previous);report("holding","sequential_change_retained_unverified_quality");}
            pending=false;recovering=false;gate.reset();continue;
        }
        gate.observe(ft,budget);
        if(gate.recover()&&!history.empty()&&(!retry.contains({0,0})||now>=retry[{0,0}])){
            previous=current;auto next=history.back();next.id=next.compute.empty()&&!next.cpu_state_cache?0:++sequence;if(apply(next)){current=next;before=ft;pending=true;recovering=true;pending_key={0,0};report("holding","restoring_detail_with_headroom");}gate.reset();continue;
        }
        if(!gate.reduce()){report(ft<=budget*1.03?"target_met":"holding","observing_target_band");continue;}
        bool changed=false;
        for(const auto& work:catalog){const auto& c=work.capabilities;arc::ComputePolicy setting;if(auto p=current.find(c.pipeline))setting=*p;setting.pipeline=c.pipeline;
            for(unsigned knob=0;knob<3&&!changed;++knob){const auto key=std::make_pair(c.pipeline,knob);if(retry.contains(key)&&now<retry[key])continue;auto next=setting;bool possible=false;
                if(knob==0&&c.coarse){if(next.x_rate==1&&next.y_rate==1){next.y_rate=2;possible=true;}else if(next.x_rate==1&&next.y_rate==2){next.x_rate=2;possible=true;}else if(s.quality_profile=="aggressive"&&next.x_rate==2&&next.y_rate==2){next.y_rate=4;possible=true;}else if(s.quality_profile=="aggressive"&&next.x_rate==2&&next.y_rate==4){next.x_rate=4;possible=true;}}
                if(knob==1&&c.samples&&next.sample_percent>25){next.sample_percent-=25;possible=true;}
                if(knob==2&&c.mips&&next.mip_steps<(s.quality_profile=="aggressive"?8u:4u)){++next.mip_steps;possible=true;}
                if(!possible)continue;next.protect_edges=false;auto candidate=current;candidate.id=++sequence;if(!candidate.replace(next))continue;
                if(apply(candidate)){previous=current;current=candidate;before=ft;pending=true;pending_key=key;changed=true;report("holding","reducing_measured_expensive_compute");}else retry[key]=now+std::chrono::seconds(30);
            }if(changed)break;
        }
        gate.reset();if(!changed)report("limited","no_currently_applicable_compute_reduction");
    }
    gpu_profile::stop();select(L"off");optimizer::approve_policy(0,0);
}
