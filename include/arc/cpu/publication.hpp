#pragma once

#include "arc/candidate_contract.hpp"
#include "arc/cpu/region_model.hpp"

namespace arc::cpu {

// These facts describe the *closed* CPU model. The DBI frontend must establish
// them by decoding the complete replaced byte range and preserving its exit.
constexpr std::uint64_t fact_register_only = 1ull << 0;
constexpr std::uint64_t fact_complete_region = 1ull << 1;
constexpr std::uint64_t fact_flags_safe = 1ull << 2;
constexpr std::uint64_t guard_code_identity = 1ull << 0;
constexpr std::uint64_t guard_module_lifetime = 1ull << 1;
constexpr std::uint64_t guard_complete_inputs = 1ull << 2;

struct CpuPublicationInput {
    std::uint64_t id{}, code_begin{}, code_end{}, original_entry{}, variant_entry{};
    CandidateFingerprint fingerprint{};
    CandidateKind kind{CandidateKind::Specialization};
    std::uint64_t analyzer_version{1};
    std::uint64_t required_guards{guard_code_identity | guard_module_lifetime | guard_complete_inputs};
    std::uint64_t dependency_scope{1};
};

[[nodiscard]] inline CandidateRecord make_cpu_candidate_record(const Analysis& analysis,
                                                                const CpuPublicationInput& input,
                                                                bool include_descriptions = false) {
    CandidateRecord record{};
    record.id = input.id;
    record.analyzer_version = input.analyzer_version;
    record.code_begin = input.code_begin;
    record.code_end = input.code_end;
    record.original_entry = input.original_entry;
    record.variant_entry = input.variant_entry;
    record.fingerprint = input.fingerprint;
    record.backend = CandidateBackend::Cpu;
    record.kind = input.kind;
    record.exactness = CandidateExactness::Exact;
    record.guard_boundary = GuardBoundary::CpuRegionEntry;
    record.execution_lifetime = ExecutionLifetime::CpuLease;
    record.required_guards = input.required_guards;
    record.dependency_scope = input.dependency_scope;
    record.required_facts = fact_register_only | fact_complete_region | fact_flags_safe;
    // CandidateRecord's default empty strings/vector do not allocate. The
    // optional descriptions belong only on an ordinary control thread: the
    // DBI analysis callback may run before a usable process CRT heap exists.
    if (include_descriptions) {
        record.supported_model = "x64 64-bit register-only acyclic straight-line region";
        record.assumptions = "DBI proved complete byte range, entry state, exit liveness, code identity, module lifetime and absence of external effects";
        record.input_effect_scope = "64-bit GPR live-ins and live-outs; no memory, calls, branches, stack writes or observable flag writes";
    }
    if (analysis.accepted()) {
        record.proven_facts = record.required_facts;
        record.completeness = EvidenceCompleteness::ProvenUnderAssumptions;
        record.validation = ValidationMethod::SemanticProof;
        record.supported_scope = true;
        record.guard_before_effects = true;
    }
    return record;
}

// Retains the shared admission checks while avoiding any dynamic allocation
// in the DBI callback. Exact CPU admission does not inspect string fields.
[[nodiscard]] inline CandidateAdmission make_cpu_pod_admission(const Analysis& analysis,
                                                               const CpuPublicationInput& input) noexcept {
    return admit_candidate(make_cpu_candidate_record(analysis, input, false));
}

[[nodiscard]] inline bool publish_cpu_candidate(PublishedCandidate& publication,
                                                const CandidateAdmission& admission,
                                                bool local_validation_passed) noexcept {
    return admission.accepted() && publication.mark_analyzed() &&
           publication.mark_eligible() && publication.prepare(admission) &&
           publication.local_validate(local_validation_passed) && publication.activate();
}

// Called on a control path after local state-oracle validation. The immutable
// projection then becomes available through allocation-free CPU leases.
[[nodiscard]] inline bool publish_cpu_candidate(PublishedCandidate& publication,
                                                const CandidateRecord& record,
                                                bool local_validation_passed) noexcept {
    const auto admission = admit_candidate(record);
    return publish_cpu_candidate(publication, admission, local_validation_passed);
}

} // namespace arc::cpu
