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
bool feedback_vrs_capable(){
    Microsoft::WRL::ComPtr<ID3D12Device> device;{std::lock_guard lock(state().mutex);device=state().telemetry_device;}
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps{};
    return device&&SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&caps,sizeof(caps)))&&caps.VariableShadingRateTier>=D3D12_VARIABLE_SHADING_RATE_TIER_2;
}
void run_target_feedback(){
    auto& s=state();optimizer::spatial_learning(false,0);optimizer::pause_spatial_probes(true);optimizer::sample_frame_state(false);
    struct StopVrs {~StopVrs(){pixel::configure(0);mirror::configure(0);}} stop_vrs;
    arc::BottleneckRouter router;arc::Bottleneck route=arc::Bottleneck::Unknown;gpu_profile::LoadEvidence load;bool cpu_cache=false,gpu_updates_allowed=false;
    arc::FrameTimePid controller;arc::FrameTimePidSample sample;arc::ComputeAllocation allocation;arc::PolicyBundle current;
    std::uint64_t context_profile_floor=gpu_profile::load_evidence().sequence;std::uint64_t sequence=0,profile=0,revision=s.surface_revision.load(),bindings=optimizer::binding_evidence_revision();
    std::vector<optimizer::WorkCandidate> catalog;std::map<std::uint64_t,double> reference_cost;
    const auto start=Clock::now();auto last_profile=start-std::chrono::seconds(30),last_control=start,last_report=start-std::chrono::seconds(2),last_catalog=start-std::chrono::seconds(2),profile_started=start;
    std::filesystem::path profile_path;bool vrs_available=false,vrs_enabled=false;
    Clock::time_point cpu_check_since{},cpu_retry_after{};std::uint64_t cpu_skipped_before{};std::string cpu_outcome="not_requested";
    auto disable=[&]{select(L"off");pixel::configure(0);mirror::configure(0);optimizer::approve_policy(0,0);current={};vrs_enabled=false;controller.reset();};
    auto report=[&](const char* phase,const char* reason){auto row=feedback_memory();row["phase"]=phase;row["reason"]=reason;row["controller"]="frame_time_pid_v1";row["target_fps"]=s.target;row["target_frame_ms"]=1000./s.target;row["frame_ms"]=period();row["active_action"]=current.id;
        row["pid"]={{"error_ms",sample.error_ms},{"filtered_frame_ms",sample.filtered_ms},{"p",sample.proportional},{"i",sample.integral},{"d",sample.derivative},{"intensity",sample.output},{"saturated",sample.saturated}};
        row["allocation"]={{"model","measured_pass_cost_with_heuristic_marginal_priors"},{"requested_saving_ms",allocation.requested_saving_ms},{"estimated_saving_ms",allocation.estimated_saved_ms},{"estimated_capacity_ms",allocation.estimated_capacity_ms},{"steps",allocation.steps}};
        row["policy_lifetime"]="until_controller_decision_stop_or_incompatible_context";row["quality_verified"]=false;row["bottleneck"]=arc::bottleneck_name(route);
        row["load_profile"]={{"sequence",load.sequence},{"age_seconds",load.completed_tick_ms?Json(double(GetTickCount64()-load.completed_tick_ms)/1000):Json(nullptr)},{"reference_frame_ms",load.frame_ms},{"present_thread_running_ms_per_frame",load.cpu_valid?Json(load.cpu_running_ms):Json(nullptr)},{"busiest_observed_gpu_queue_ms_per_frame",load.gpu_valid?Json(load.gpu_queue_busy_ms):Json(nullptr)},{"scope","CPU_running_on_present_thread_and_GPU_queue_lower_bound_not_complete_critical_path"}};
        row["cpu_state_cache"]=current.cpu_state_cache;row["cpu_cache_outcome"]=cpu_outcome;const auto cpu_counts=optimizer::cpu_cache_counters();row["cpu_redundant_calls_skipped_total"]=cpu_counts.skipped;
        row["gpu_policy_held"]=!gpu_updates_allowed;
        row["unsupported_cpu_actuators"]={"game_scene_traversal","game_lod_selection","game_worker_parallelization"};
        row["targets"]=Json::array();for(const auto& p:current.compute)row["targets"].push_back({{"pipeline",p.pipeline},{"rate",{p.x_rate,p.y_rate}},{"mip_steps",p.mip_steps},{"sample_percent",p.sample_percent},{"comparison_taps",p.comparison_taps},{"zero_factor",p.zero_factor}});
        const auto pixel_budget=pixel::requested_budget();row["pixel_mip_half_steps"]=pixel_budget[0];row["pixel_comparison_taps"]=pixel_budget[1];row["pixel_independent_sample_percent"]=pixel_budget[2];row["pixel_variants_ready"]=pixel::ready();row["vrs_requested"]=vrs_enabled;row["vrs_modified_draw_submissions_total"]=mirror::modified_draws();row["vrs_hardware_available"]=vrs_available;
        row["unsupported_actuators"]={"mesh_simplification","shadow_resource_resizing","arbitrary_rt_rewrite","temporal_reprojection","texture_residency_control"};
        double cost=0;for(const auto& c:catalog)cost+=c.gpu_ms_per_window;row["profiled_supported_compute_ms"]=catalog.empty()?Json(nullptr):Json(cost);row["profile_age_seconds"]=std::chrono::duration<double>(Clock::now()-last_profile).count();publish(std::move(row));last_report=Clock::now();
    };
    report("warmup","pid_discovery");
    while(!s.cancel){
        if(s.maximum_seconds&&Clock::now()-start>std::chrono::seconds(s.maximum_seconds))break;
        if(!wait_frames(1)){if(s.cancel)break;if(vrs_enabled)mirror::keep_alive();report("inactive","policy_retained_without_present_progress");continue;}
        if(auto requested=s.requested_target.exchange(0);requested>0){s.target=requested;controller.reset(sample.output);}
        const auto now=Clock::now();
        if(revision!=s.surface_revision.load()||bindings!=optimizer::binding_evidence_revision()){
            const auto retained_intensity=sample.output;const bool same_surface=revision==s.surface_revision.load();bool rebound=false;
            optimizer::reset_binding_evidence();
            if(same_surface){std::lock_guard lock(s.policy_mutex);if(!s.cancel&&optimizer::configure_bundle(current,true,0,0)){optimizer::approve_policy(current.id,std::numeric_limits<std::uint64_t>::max());rebound=true;}}
            if(!rebound){disable();controller.reset(retained_intensity);sample.output=retained_intensity;reference_cost.clear();catalog.clear();}
            router.reset();route=arc::Bottleneck::Unknown;context_profile_floor=gpu_profile::load_evidence().sequence;
            revision=s.surface_revision.load();bindings=optimizer::binding_evidence_revision();last_profile=now-std::chrono::seconds(30);report("warmup",rebound?"policy_retained_with_submission_time_binding_checks":"technical_context_requires_rebind");
        }
        // Discovery is asynchronous. Keep running the PID while GPU profiling
        // completes, rather than blocking the controller on a file wait.
        if(!profile_path.empty()&&std::filesystem::exists(profile_path)){profile_path.clear();last_catalog=start;}
        if(!profile_path.empty()&&now-profile_started>std::chrono::seconds(12)){gpu_profile::stop();profile_path.clear();}
        if(profile_path.empty()&&now-last_profile>std::chrono::seconds(2)){
            const auto path=s.directory/(L"pid-profile-"+std::to_wstring(++profile)+L".json");
            if(profile>3){std::error_code ec;std::filesystem::remove(s.directory/(L"pid-profile-"+std::to_wstring(profile-3)+L".json"),ec);}
            if(gpu_profile::request(path.wstring(),static_cast<UINT>(std::clamp(std::ceil(300./std::max(1.,period())),16.,128.)))){profile_path=path;profile_started=now;}last_profile=now;
        }
        if(now-last_catalog>=std::chrono::seconds(1)){
            catalog=optimizer::candidate_catalog();for(const auto& c:catalog)reference_cost.try_emplace(c.capabilities.pipeline,c.gpu_ms_per_window);
            vrs_available=feedback_vrs_capable();last_catalog=now;
        }
        if(now-last_control<std::chrono::milliseconds(250))continue;
        const auto dt=std::chrono::duration<double>(now-last_control).count();last_control=now;
        load=gpu_profile::load_evidence();if(load.sequence<=context_profile_floor){load.cpu_valid=load.gpu_valid=false;load.frame_ms=0;}const auto prior_route=route;
        route=router.observe({load.sequence,load.frame_ms,load.cpu_running_ms,load.gpu_queue_busy_ms,load.completed_tick_ms?double(GetTickCount64()-load.completed_tick_ms)/1000:999.,load.cpu_valid,load.gpu_valid,1000./s.target});
        const auto permission=arc::gpu_permission(route,period(),1000./s.target,sample.output,!catalog.empty()||vrs_available||pixel::available());const bool drive_gpu=permission.update;const bool was_driving_gpu=gpu_updates_allowed;gpu_updates_allowed=drive_gpu;
        if((route==arc::Bottleneck::CpuThread||route==arc::Bottleneck::Mixed)&&!cpu_cache&&now>=cpu_retry_after){cpu_cache=true;cpu_check_since=now;cpu_skipped_before=optimizer::cpu_cache_counters().skipped;cpu_outcome="observing_redundant_call_elimination";}
        if(cpu_cache&&cpu_check_since!=Clock::time_point{}&&now-cpu_check_since>=std::chrono::seconds(2)){
            if(optimizer::cpu_cache_counters().skipped==cpu_skipped_before){cpu_cache=false;cpu_retry_after=now+std::chrono::seconds(30);cpu_outcome="no_redundant_calls_disabled";}
            else cpu_outcome="retained_calls_eliminated_not_causal_speedup_proof";
            cpu_check_since={};
        }
        if(drive_gpu){if(prior_route!=route||!was_driving_gpu)controller.reset(sample.output);sample=controller.step(period(),1000./s.target,dt,permission.maximum);if(!sample.valid){disable();continue;}}
        else {sample.error_ms=period()-1000./s.target;sample.filtered_ms=period();}
        std::vector<arc::ComputeFeedbackTarget> targets;for(const auto& work:catalog){const auto& c=work.capabilities;targets.push_back({c.pipeline,reference_cost[c.pipeline],c.coarse,c.samples,c.mips,c.comparison,c.zero});}
        if(drive_gpu)allocation=arc::allocate_compute_budget(targets,sample.output,s.quality_profile=="aggressive");
        auto desired=drive_gpu?allocation.policy:current;desired.cpu_state_cache=cpu_cache;
        bool changed=false;
        if(desired.compute!=current.compute||desired.cpu_state_cache!=current.cpu_state_cache){
            desired.id=desired.compute.empty()&&!desired.cpu_state_cache?0:++sequence;
            std::lock_guard lock(s.policy_mutex);if(s.cancel)break;
            if(optimizer::configure_bundle(desired,true,0,0)){optimizer::approve_policy(desired.id,std::numeric_limits<std::uint64_t>::max());current=std::move(desired);changed=true;}
        }
        const bool desired_vrs=drive_gpu?vrs_available&&(vrs_enabled?sample.output>.2:sample.output>.35):vrs_enabled;
        if(desired_vrs!=vrs_enabled){
            std::lock_guard lock(s.policy_mutex);if(s.cancel)break;
            if(!desired_vrs){mirror::configure(0);vrs_enabled=false;changed=true;}
            else if(hooks::begin_raster_observation()){vrs_enabled=mirror::configure(D3D12_SHADING_RATE_2X2);hooks::end_raster_observation();changed=vrs_enabled;}
        }
        if(drive_gpu){const auto steps=unsigned(std::clamp(sample.output,0.,1.)*(s.quality_profile=="aggressive"?8.:4.));const unsigned taps=sample.output>=.5?9:0,percent=sample.output>=.75?25:sample.output>=.5?50:sample.output>=.25?75:100;if(pixel::requested_budget()!=std::array<unsigned,3>{steps,taps,percent}){std::lock_guard lock(s.policy_mutex);if(!s.cancel&&hooks::begin_raster_observation()){pixel::configure(steps,taps,percent);hooks::end_raster_observation();}}}
        if(vrs_enabled)mirror::keep_alive();
        if(now-last_report>=std::chrono::milliseconds(500)){
            const bool met=sample.filtered_ms<=1.03*1000./s.target;
            report(met?"target_met":sample.output>=.999?"limited":"holding",changed?"bottleneck_policy_updated":!drive_gpu?"gpu_policy_retained_waiting_for_relevant_evidence":met?"pid_tracking_target":sample.output>=.999?"supported_actuators_saturated":"pid_tracking_error");
        }
    }
    gpu_profile::stop();disable();
}
