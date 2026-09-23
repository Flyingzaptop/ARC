// Included within the client implementation namespace.
// Timing is closed only at the exact decoded prefix boundary, including the
// redirect path. No instruction outside the admitted prefix contributes to O.
void cost_boundary(app_pc pc) {
    auto* t=static_cast<ThreadState*>(drmgr_get_tls_field(dr_get_current_drcontext(),tls_slot));
    if(!t || !t->pending.active || t->pending.end!=pc) return;
    struct FpScope { alignas(16) byte state[DR_FPSTATE_BUF_SIZE]{};
        FpScope(){proc_save_fpstate(state);_mm_setcsr(0x1f80);} ~FpScope(){proc_restore_fpstate(state);} } fp;
    const auto ended=cost_clock.now();
    const auto pending=t->pending; t->pending.active=false;
    if(!dr_mutex_trylock(candidate_lock)) return;
    if(pending.id<candidate_count.load() && candidates[pending.id].generation==pending.generation) {
        auto& c=candidates[pending.id]; auto& policy=t->policy[pending.id];
        if(policy.generation()==pending.generation && ended>pending.started && c.active.load()) {
            policy.record(pending.action,pending.generation,cost_clock.ns(ended-pending.started),pending.actuated);
            for(unsigned i=0;i<4;++i) {
                c.cost_sample_counts[i]=policy.samples()[i].count;
                c.cost_actuations[i]=policy.samples()[i].actuations;
                const auto stats=policy.statistics(i);
                c.cost_medians[i]=stats[0]; c.cost_spreads[i]=stats[1];
            }
            const auto tracked=cost_clock.now();
            if(tracked>ended) {
                c.tracking_ticks+=tracked-ended;
                policy.account_tracking(cost_clock.ns(tracked-ended));
            }
            c.cost_decision=policy.decision();
            c.cost_snapshot_tid=static_cast<unsigned>(dr_get_thread_id(dr_get_current_drcontext()));
            ++c.timed_spans;
        } else ++c.timing_drops;
    }
    dr_mutex_unlock(candidate_lock);
}
