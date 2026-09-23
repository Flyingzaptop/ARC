// Included within the client implementation namespace.
bool same_code(const Decoded& a,const Decoded& b) {
    return a.start==b.start && a.byte_count==b.byte_count &&
        a.code_hash==b.code_hash &&
        std::memcmp(a.original_bytes.data(),b.original_bytes.data(),a.byte_count)==0;
}
void admit_slot(unsigned id,const Decoded& d,std::uint64_t initial_calls=0,
                std::uint64_t decode_ticks=0,std::uint64_t recent_hits=0) {
    const auto admission_started=cost_clock.now();
    auto& c=candidates[id];
    // The terminal publication is never reopened. Replacement constructs a
    // different publication after every callback and lease has left the slot.
    if (c.generation) {
        capture->forget_novelty(c.generation);
        c.publication.invalidate(arc::CandidateReason::Superseded);
        c.~Candidate();
        new (&c) Candidate();
        ++replacements;
    }
    c.decoded=d; c.analysis=analyze(d.region);
    c.module_start=main_start; c.module_end=main_end;
    c.generation=next_generation++;
    c.calls.store(initial_calls,std::memory_order_relaxed);
    c.last_seen=observation_clock; c.recent_hits=recent_hits;
    c.fingerprint.content=d.code_hash ? d.code_hash : 1;
    c.fingerprint.code_generation=c.generation;
    c.fingerprint.context=reinterpret_cast<std::uintptr_t>(main_start);
    CpuPublicationInput publication{};
    publication.id=c.generation; publication.code_begin=reinterpret_cast<std::uintptr_t>(d.start);
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
    const auto admission_finished=cost_clock.now();
    c.discovery_ticks=decode_ticks + (admission_finished>admission_started ?
        admission_finished-admission_started : 0);
    ++discovered;
    if (mode==Mode::Study) capture->note_novelty(c.generation);
}
unsigned replacement_slot(std::uint64_t activity) {
    const unsigned count=candidate_count.load(std::memory_order_relaxed);
    if (count<max_regions) return count;
    unsigned chosen=max_regions;
    std::uint64_t lowest=UINT64_MAX;
    for (unsigned i=0;i<count;++i) {
        auto& c=candidates[i];
        if (c.publication.outstanding()) continue;
        const auto age=observation_clock-c.last_seen;
        const auto decay=age/64;
        const auto hits=decay>=c.recent_hits ? 0 : c.recent_hits-decay;
        if (hits<lowest) { lowest=hits; chosen=i; }
    }
    return chosen<max_regions && activity>lowest ? chosen : max_regions;
}
void on_observed(unsigned index,std::uint64_t expected_serial,app_pc start) {
    if (!dr_mutex_trylock(candidate_lock)) return;
    unsigned target=max_regions; std::uint64_t generation{};
    bool stale=true;
    if (index<max_observations) {
        auto& o=observations[index];
        if (o.serial==expected_serial && expected_serial) {
            stale=false;
            const auto tick=++observation_clock;
            const auto age=tick-o.last_seen;
            const auto decay=age/64;
            o.recent_hits=decay>=o.recent_hits ? 1 :
                (o.recent_hits-decay<256 ? o.recent_hits-decay+1 : 256);
            o.last_seen=tick;
            ++o.calls; ++observations_seen;
            if (o.candidate<max_regions && candidates[o.candidate].generation==o.candidate_generation) {
                target=o.candidate; generation=o.candidate_generation;
            } else if (o.recent_hits>=hot_threshold && !stopped.load(std::memory_order_acquire)) {
                const unsigned chosen=replacement_slot(o.recent_hits);
                if (chosen<max_regions) {
                    admit_slot(chosen,o.decoded,o.calls,o.decode_ticks,o.recent_hits);
                    if (chosen==candidate_count.load(std::memory_order_relaxed))
                        candidate_count.store(chosen+1,std::memory_order_release);
                    o.candidate=chosen; o.candidate_generation=candidates[chosen].generation;
                    target=chosen; generation=o.candidate_generation;
                }
            }
        }
    }
    dr_mutex_unlock(candidate_lock);
    // A fixed observation slot may have aged out while its translated block
    // remains cached. Ask DR to rebuild that block, so hot late code can enter
    // the bounded observation pool again. The stale callback never executes a
    // candidate that later occupied its old index.
    if (stale && start>=main_start && start<main_end)
        dr_delay_flush_region(start,1,0,nullptr);
    if (target<max_regions) on_region(target,generation,start);
}
