#include "arc/candidate_contract.hpp"

#include <cassert>
#include <atomic>
#include <thread>
#include <type_traits>

using namespace arc;

static CandidateRecord exact_record() {
    CandidateRecord r;
    r.id = 17; r.analyzer_version = 3; r.code_begin = 0x1000; r.code_end = 0x1100;
    r.backend = CandidateBackend::Cpu; r.kind = CandidateKind::Specialization;
    r.fingerprint = {99, 2, 5, 7, 11, 13};
    r.completeness = EvidenceCompleteness::ProvenUnderAssumptions;
    r.validation = ValidationMethod::SemanticProof;
    r.required_facts = 0b011; r.proven_facts = 0b011;
    r.required_guards = 0b101; r.dependency_scope = 0b010;
    r.guard_boundary = GuardBoundary::CpuRegionEntry;
    r.guard_before_effects = r.supported_scope = true;
    r.original_entry = 0x1000; r.variant_entry = 0x2000;
    r.execution_lifetime = ExecutionLifetime::CpuLease;
    return r;
}

int main() {
    static_assert(!std::is_copy_constructible_v<CandidateLease>);
    auto r = exact_record();
    auto admitted = admit_candidate(r);
    assert(admitted.accepted());
    assert(!admitted.projection.quality_verified);
    assert(admitted.projection.execution_lifetime == ExecutionLifetime::CpuLease);

    r.completeness = EvidenceCompleteness::Unknown;
    assert(admit_candidate(r).reason == CandidateReason::UnknownDependency);
    r.completeness = EvidenceCompleteness::Observed;
    assert(admit_candidate(r).reason == CandidateReason::IncompleteEvidence);
    r.completeness = EvidenceCompleteness::ProvenUnderAssumptions;
    r.proven_facts = 0b001;
    assert(admit_candidate(r).reason == CandidateReason::MissingProofObligation);
    r.proven_facts = r.required_facts;
    r.validation = ValidationMethod::Samples;
    assert(admit_candidate(r).reason == CandidateReason::MissingSemanticProof);

    r.exactness = CandidateExactness::Approximate;
    assert(admit_candidate(r).reason == CandidateReason::MissingQualityEvidence);
    r.experimental_unverified = true;
    assert(admit_candidate(r).accepted());
    assert(!admit_candidate(r).projection.quality_verified);
    r.experimental_unverified = false;
    r.validation = ValidationMethod::ScopedQualityTrial;
    r.quality_accepted = true;
    r.quality_scope = "scene 8"; r.quality_reference = "original frame 12";
    assert(admit_candidate(r).projection.quality_verified);

    PublishedCandidate policy;
    assert(!policy.try_acquire(admitted.projection.fingerprint, 0b111));
    assert(policy.mark_analyzed() && policy.mark_eligible());
    assert(policy.prepare(admitted) && policy.local_validate(true) && policy.activate());
    assert(policy.lifecycle() == CandidateLifecycle::Active);
    auto wrong = admitted.projection.fingerprint;
    ++wrong.resource_generation;
    assert(!policy.try_acquire(wrong, 0b111));
    assert(!policy.try_acquire(admitted.projection.fingerprint, 0b001));
    auto first = policy.try_acquire(admitted.projection.fingerprint, 0b111);
    auto second = policy.try_acquire(admitted.projection.fingerprint, 0b111);
    assert(first && second && policy.outstanding() == 2);
    policy.event_loss(0b100, true); // unrelated correctness scope
    assert(policy.lifecycle() == CandidateLifecycle::Active);
    policy.event_loss(0b010, false); // diagnostic loss only
    assert(policy.diagnostic_loss() == 1 && policy.lifecycle() == CandidateLifecycle::Active);
    policy.event_loss(0b010, true);
    assert(policy.reason() == CandidateReason::CorrectnessEventLoss);
    assert(policy.lifecycle() == CandidateLifecycle::Invalidated);
    assert(!policy.try_acquire(admitted.projection.fingerprint, 0b111));
    first.reset();
    assert(policy.outstanding() == 1 && policy.lifecycle() == CandidateLifecycle::Invalidated);
    assert(second->variant_entry == 0x2000); // storage remains alive for existing user
    second.reset();
    assert(policy.outstanding() == 0 && policy.lifecycle() == CandidateLifecycle::Retired);

    PublishedCandidate budget;
    budget.reject(CandidateReason::BudgetExceeded);
    assert(budget.lifecycle() == CandidateLifecycle::BudgetExhausted);
    assert(!budget.mark_analyzed());
    PublishedCandidate failed;
    assert(failed.mark_analyzed() && failed.mark_eligible() && failed.prepare(admitted));
    assert(!failed.local_validate(false));
    assert(failed.lifecycle() == CandidateLifecycle::Rejected);

    // Either ordering of activate/invalidate must leave a permanently closed
    // gate. This exercises the old closed -> closed ABA between both calls.
    for (unsigned i = 0; i < 256; ++i) {
        PublishedCandidate raced;
        assert(raced.mark_analyzed() && raced.mark_eligible());
        assert(raced.prepare(admitted) && raced.local_validate(true));
        std::atomic<bool> start{false};
        std::thread activator([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)raced.activate();
        });
        std::thread invalidator([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            raced.invalidate(CandidateReason::Superseded);
        });
        start.store(true, std::memory_order_release);
        activator.join(); invalidator.join();
        assert(raced.lifecycle() == CandidateLifecycle::Retired);
        assert(raced.outstanding() == 0);
        assert(!raced.try_acquire(admitted.projection.fingerprint, 0b111));
    }

    PublishedCandidate cancelled;
    assert(cancelled.mark_analyzed() && cancelled.mark_eligible());
    assert(cancelled.prepare(admitted) && cancelled.local_validate(true));
    cancelled.reject(CandidateReason::BudgetExceeded);
    assert(cancelled.lifecycle() == CandidateLifecycle::BudgetExhausted);
    assert(!cancelled.activate());
    assert(!cancelled.try_acquire(admitted.projection.fingerprint, 0b111));
}
