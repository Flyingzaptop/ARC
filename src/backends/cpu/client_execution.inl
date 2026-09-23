// Included within the client implementation namespace.
void on_region(unsigned id,std::uint64_t expected_generation,app_pc original_start) {
    struct FpScope {
        alignas(16) byte state[DR_FPSTATE_BUF_SIZE]{};
        FpScope() { proc_save_fpstate(state); _mm_setcsr(0x1f80); }
        ~FpScope() { proc_restore_fpstate(state); }
        void before_redirect() { proc_restore_fpstate(state); }
    } fp;
    const auto entered=cost_clock.now();
    check_stop_control();
    if (!dr_mutex_trylock(candidate_lock)) return;
    if (id>=candidate_count.load(std::memory_order_acquire) ||
        candidates[id].generation!=expected_generation) {
        dr_mutex_unlock(candidate_lock);
        if(!stopped.load() && !conflict.load() && original_start>=main_start && original_start<main_end)
            dr_delay_flush_region(original_start,1,0,nullptr);
        return;
    }
    Candidate& c=candidates[id];
    struct Unlock { bool held{true}; ~Unlock() { if (held) dr_mutex_unlock(candidate_lock); } } unlock;
    const auto tick=++observation_clock;
    const auto age=tick-c.last_seen;
    const auto decay=age/64;
    c.recent_hits=decay>=c.recent_hits ? 1 :
        (c.recent_hits-decay<256 ? c.recent_hits-decay+1 : 256);
    c.last_seen=tick;
    const auto calls=c.calls.fetch_add(1,std::memory_order_relaxed)+1;
    ++total_calls;
    if(!stopped.load() && !conflict.load() && !c.active.load()) {
        dr_mem_info_t info{};
        const bool executable=dr_query_memory_ex(c.decoded.start,&info) &&
            (info.prot&(DR_MEMPROT_EXEC|DR_MEMPROT_WRITE))==DR_MEMPROT_EXEC;
        unlock.held=false; dr_mutex_unlock(candidate_lock);
        if(executable) dr_delay_flush_region(original_start,1,0,nullptr);
        return;
    }
    if (mode==Mode::Study && calls>=hot_threshold && c.sample_status.load(std::memory_order_acquire)==0) {
        unsigned expected=0;
        if (c.sample_status.compare_exchange_strong(expected,1)) {
            const auto observation=capture->try_observe(c.generation,
                c.decoded.region.count,c.decoded.byte_count+sizeof(State),c.analysis.live_in_mask,c.decoded.code_hash);
            if (observation!=DiscoveryCapture::Observation::Started &&
                observation!=DiscoveryCapture::Observation::Recorded) {
                c.sample_status.store(0,std::memory_order_release);
                return;
            }
            dr_mcontext_t mc{sizeof(mc),DR_MC_ALL};
            if (dr_get_mcontext(dr_get_current_drcontext(),&mc)) {
                read_state(mc,c.sample);
                c.sample_status.store(capture->finish(true)?2:0,std::memory_order_release);
            } else {
                capture->finish(false);
                c.sample_status.store(0,std::memory_order_release);
            }
        }
    }
    if (mode!=Mode::Apply || calls<hot_threshold || stopped.load(std::memory_order_acquire) ||
        conflict.load(std::memory_order_acquire) || !c.active.load(std::memory_order_acquire)) return;
    void* dc=dr_get_current_drcontext();
    auto* thread=static_cast<ThreadState*>(drmgr_get_tls_field(dc,tls_slot));
    if (!thread) return;
    Actuator selected=actuator;
    if(actuator==Actuator::Auto) {
        auto& policy=thread->policy[id];
        if(policy.generation()!=c.generation)
            policy.reset(c.generation,cost_clock.pair_cost_ns,cost_clock.ns(c.discovery_ticks));
        if(!entered || !cost_clock.query) policy.clock_unavailable();
        const auto action=policy.next();
        c.cost_decision=policy.decision();
        if(policy.collecting()) {
            if(thread->pending.active) ++c.timing_drops;
            thread->pending={true,false,id,id%4,c.generation,entered,c.decoded.end,action};
        }
        if(action==CostAction::Original) return;
        selected=action==CostAction::Specialize ? Actuator::Specialize :
            action==CostAction::Memo ? Actuator::Memo : Actuator::Incremental;
    }
    if (!code_matches(c)) { c.active.store(false,std::memory_order_release);
        c.publication.invalidate(arc::CandidateReason::GenerationChanged); ++code_changed; return; }
    auto lease=c.publication.try_acquire(c.fingerprint,
        guard_code_identity|guard_module_lifetime|guard_complete_inputs);
    if (!lease) return;
    dr_mcontext_t mc{sizeof(mc),DR_MC_ALL};
    if (!dr_get_mcontext(dc,&mc)) return;
    State state{}; read_state(mc,state);
    RunResult result{};
    if (selected==Actuator::Specialize) {
        // A specialization is published from the first value observation in
        // this thread. The guard checks all required live-ins on every use.
        auto& slot=thread->slot[id%4];
        auto& cache=slot.get(id,c.generation,c.analysis);
        if (!slot.spec.valid) {
            slot.spec=specialize(c.analysis,state,c.analysis.live_in_mask,c.generation);
            ++c.misses; ++total_misses;
            return; // original instructions establish the first observation
        }
        bool guard=false;
        result=execute_specialized(c.analysis,slot.spec,state,c.generation,guard);
        if (guard) (++c.guards,++total_guards); else { ++c.misses; ++total_misses; return; }
        (void)cache;
    } else {
        auto& cache=thread->slot[id%4].get(id,c.generation,c.analysis);
        RegionCache::Action action=(selected==Actuator::Incremental) ? RegionCache::Action::Incremental : RegionCache::Action::Memo;
        result=cache.run(action,state,c.generation);
        if (result.reused_nodes) (++c.guards,++total_guards); else { ++c.misses; ++total_misses; return; }
    }
    if (!result.output_mask) return;
    c.reused_nodes.fetch_add(result.reused_nodes,std::memory_order_relaxed);
    c.dirty_nodes.fetch_add(result.evaluated_nodes,std::memory_order_relaxed);
    write_state(mc,result);
    mc.pc=c.decoded.end;
    ++c.executions; ++total_executions;
    if(actuator==Actuator::Auto) {
        if(thread->policy[id].collecting()) ++c.auto_trial_executions;
        else ++c.auto_policy_executions;
    }
    if(thread->pending.active && thread->pending.id==id && thread->pending.generation==c.generation) thread->pending.actuated=true;
    // Candidate/model storage is fixed for process lifetime. The immutable
    // projection has been consumed; release before non-returning redirect.
    lease.reset();
    unlock.held=false;
    dr_mutex_unlock(candidate_lock);
    fp.before_redirect();
    // Redirection is the execution boundary: on admission miss we return to
    // untouched original instructions; on hit the entire closed prefix is skipped.
    dr_redirect_execution(&mc);
}
