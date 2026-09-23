// Included within the client implementation namespace.
void exit_event() {
    ending.store(true,std::memory_order_release);
    if (stop_map) dr_unmap_file(stop_map,stop_map_size);
    if (stop_file!=INVALID_FILE) dr_close_file(stop_file);
    if (!output_path[0]) return;
    const file_t f=dr_open_file(output_path,DR_FILE_WRITE_OVERWRITE);
    if (f==INVALID_FILE) return;
    const auto calls=total_calls.load(),exec=total_executions.load(),
        guards=total_guards.load(),misses=total_misses.load();
    const auto cap=capture->snapshot();
    const auto& limits=capture->config();
    dr_fprintf(f,"{\"capture_limit_events\":%llu,\"capture_limit_bytes\":%llu,\"capture_limit_ms\":%llu,\"capture_concurrency\":%u,",
        (unsigned long long)limits.capture_events,(unsigned long long)limits.capture_bytes,
        (unsigned long long)limits.capture_window.count(),limits.cpu_captures);
    dr_fprintf(f,"\"backend\":\"dynamorio-11.3.0\",\"mode\":\"%s\",\"actuator\":\"%s\",\"pid\":%u,\"start_utc_us_since_1601\":%llu,\"regions_discovered\":%u,\"regions_rejected\":%llu,\"calls\":%llu,\"executions\":%llu,\"guard_hits\":%llu,\"guard_misses\":%llu,\"code_changed\":%llu,\"protection_events\":%llu,\"range_invalidations\":%llu,\"candidate_replacements\":%llu,\"observed_calls\":%llu,\"module_unloads\":%llu,\"stopped\":%s,\"stop_control_available\":%s,\"dx12_conflict\":%s,\"capture_started\":%llu,\"capture_completed\":%llu,\"capture_incomplete\":%llu,\"capture_events\":%llu,\"capture_bytes\":%llu,\"capture_rejected\":%llu,\"auto_application_disabled\":%s,\"regions\":[",
        mode==Mode::Apply?"apply":mode==Mode::Study?"study":"neutral",
        actuator==Actuator::Memo?"memo":actuator==Actuator::Incremental?"incremental":actuator==Actuator::Specialize?"specialize":"auto",
        static_cast<unsigned>(dr_get_process_id()),(unsigned long long)start_us,
        static_cast<unsigned>(discovered.load()),(unsigned long long)rejected.load(),(unsigned long long)calls,(unsigned long long)exec,
        (unsigned long long)guards,(unsigned long long)misses,(unsigned long long)code_changed.load(),
        (unsigned long long)protection_events.load(),(unsigned long long)range_invalidations.load(),
        (unsigned long long)replacements.load(),(unsigned long long)observations_seen.load(),
        (unsigned long long)module_unloads.load(),
        stopped.load()?"true":"false",stop_control_available?"true":"false",conflict.load()?"true":"false",
        (unsigned long long)cap.started,(unsigned long long)cap.completed,(unsigned long long)cap.incomplete,
        (unsigned long long)cap.events,(unsigned long long)cap.bytes,(unsigned long long)cap.budget.rejected_captures,
        "false");
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
        dr_fprintf(f,"],\"cost_aggregation\":\"last_thread_snapshot\",\"cost_thread_id\":%u,\"auto_trial_executions\":%llu,\"auto_policy_executions\":%llu,\"action_median_ns\":[",
            c.cost_snapshot_tid,(unsigned long long)c.auto_trial_executions,(unsigned long long)c.auto_policy_executions);
        for(unsigned n=0;n<4;++n) {
            if(n) dr_fprintf(f,",");
            if(c.cost_sample_counts[n]>=RuntimeCost::samples_per_action)
                dr_fprintf(f,"%llu",(unsigned long long)c.cost_medians[n]);
            else dr_fprintf(f,"null");
        }
        dr_fprintf(f,"],\"tracking_ns\":%llu,\"discovery_ns\":%llu,\"timer_pair_ns\":%llu,\"cost_scope\":", 
            (unsigned long long)cost_clock.ns(c.tracking_ticks),(unsigned long long)cost_clock.ns(c.discovery_ticks),
            (unsigned long long)cost_clock.pair_cost_ns);
        dr_fprintf(f,"\"within_DBI_entry_to_prefix_exit\",\"timed_spans\":%llu,\"timing_drops\":%llu,\"cost_action\":%u,\"cost_reason\":%u,\"cost_samples\":[%u,%u,%u,%u]",
            (unsigned long long)c.timed_spans,(unsigned long long)c.timing_drops,
            (unsigned)c.cost_decision.action,(unsigned)c.cost_decision.reason,
            c.cost_sample_counts[0],c.cost_sample_counts[1],c.cost_sample_counts[2],c.cost_sample_counts[3]);
        const bool measured=c.cost_decision.reason==CostReason::Profitable ||
            (c.cost_decision.reason==CostReason::NoBenefit && c.cost_sample_counts[0]==RuntimeCost::samples_per_action);
        auto metric=[&](const char* name,double value) {
            dr_fprintf(f,",\"%s\":",name);
            if(measured) dr_fprintf(f,"%llu",(unsigned long long)value); else dr_fprintf(f,"null");
        };
        metric("original_ns",c.cost_decision.original_ns); metric("selected_ns",c.cost_decision.selected_ns);
        metric("net_saved_ns",c.cost_decision.net_saved_ns); metric("noise_ns",c.cost_decision.noise_ns);
        metric("extra_cost_ns",c.cost_decision.extra_cost_ns);
        dr_fprintf(f,",\"raw_hex\":\"");
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
