#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace arc {

enum class CandidateBackend : std::uint8_t { Cpu, Dx12 };
enum class CandidateKind : std::uint8_t { Specialization, ExactReuse, Incremental, ShaderTransform, ApproximateShader };
enum class CandidateExactness : std::uint8_t { Exact, Approximate };
enum class EvidenceCompleteness : std::uint8_t { Unknown, Observed, ProvenUnderAssumptions };
enum class ValidationMethod : std::uint8_t { None, Samples, SemanticProof, ScopedQualityTrial };
enum class CandidateLifecycle : std::uint8_t {
    Discovered, Analyzed, Eligible, Prepared, LocallyValidated, Active,
    Invalidated, Retired, Rejected, BudgetExhausted
};
// Stable values for telemetry and persisted evidence. Add new reasons only at the end.
enum class CandidateReason : std::uint8_t {
    Accepted, InvalidIdentity, UnsupportedScope, UnknownDependency,
    IncompleteEvidence, MissingProofObligation, MissingSemanticProof,
    MissingQualityEvidence, MissingGuard, InvalidTransition,
    LocalValidationFailed, CorrectnessEventLoss, GenerationChanged,
    ContextChanged, UserStopped, BudgetExceeded, Superseded
};
enum class GuardBoundary : std::uint8_t { None, CpuRegionEntry, Dx12Record, Dx12Submit };
enum class ExecutionLifetime : std::uint8_t { CpuLease, GpuExternalFence };

struct CandidateFingerprint {
    std::uint64_t content{}, code_generation{}, allocation_generation{};
    std::uint64_t resource_generation{}, dependency_epoch{}, context{};
    [[nodiscard]] bool operator==(const CandidateFingerprint&) const noexcept = default;
};

// Detailed, cold-path evidence. An observed corpus remains Observed even if
// every sampled output matched; only a supported semantic argument can prove
// an exact replacement under named assumptions.
struct CandidateRecord {
    std::uint64_t id{}, analyzer_version{}, code_begin{}, code_end{};
    CandidateBackend backend{CandidateBackend::Dx12};
    CandidateKind kind{CandidateKind::ShaderTransform};
    CandidateExactness exactness{CandidateExactness::Exact};
    CandidateFingerprint fingerprint{};
    EvidenceCompleteness completeness{EvidenceCompleteness::Unknown};
    ValidationMethod validation{ValidationMethod::None};
    std::uint64_t required_facts{}, proven_facts{}, observed_facts{};
    std::uint64_t required_guards{}; // runtime facts supplied at the use boundary
    std::uint64_t dependency_scope{}; // bit mask; zero means all scopes
    GuardBoundary guard_boundary{GuardBoundary::None};
    bool supported_scope{}, guard_before_effects{}, quality_accepted{};
    bool experimental_unverified{}; // approximate trial only; never quality_verified
    ExecutionLifetime execution_lifetime{ExecutionLifetime::GpuExternalFence};
    std::uint64_t original_entry{}, variant_entry{};
    std::uint64_t variant_parameters[4]{};
    std::string supported_model, assumptions, corpus_identity;
    std::string input_effect_scope, quality_scope, quality_reference;
    double guard_cost_ns{}, restore_cost_ns{}, transform_cost_ns{};
    double tracking_cost_ns{}, amortized_discovery_cost_ns{};
    std::uint64_t prepared_count{}, executed_count{};
    std::vector<CandidateReason> fallback_reasons;
};

// Fixed-size immutable projection read at a call/draw boundary. It contains
// no owned strings, maps, graph, JSON, or allocation-bearing members.
struct ExecutableProjection {
    std::uint64_t id{}, analyzer_version{};
    CandidateBackend backend{CandidateBackend::Dx12};
    CandidateKind kind{CandidateKind::ShaderTransform};
    CandidateExactness exactness{CandidateExactness::Exact};
    GuardBoundary guard_boundary{GuardBoundary::None};
    ExecutionLifetime execution_lifetime{ExecutionLifetime::GpuExternalFence};
    bool quality_verified{};
    CandidateFingerprint fingerprint{};
    std::uint64_t required_guards{};
    std::uint64_t dependency_scope{};
    std::uint64_t original_entry{}, variant_entry{};
    std::uint64_t variant_parameters[4]{};
};

struct CandidateAdmission {
    CandidateReason reason{CandidateReason::InvalidIdentity};
    ExecutableProjection projection{};
    [[nodiscard]] bool accepted() const noexcept { return reason == CandidateReason::Accepted; }
};

[[nodiscard]] inline CandidateAdmission admit_candidate(const CandidateRecord& r) noexcept {
    CandidateAdmission out{};
    if (!r.id || !r.analyzer_version || !r.fingerprint.content ||
        !r.fingerprint.code_generation || !r.original_entry || !r.variant_entry ||
        (r.backend == CandidateBackend::Cpu && !(r.code_begin < r.code_end)))
        return out;
    if (!r.supported_scope) { out.reason = CandidateReason::UnsupportedScope; return out; }
    if (r.completeness == EvidenceCompleteness::Unknown) {
        out.reason = CandidateReason::UnknownDependency; return out;
    }
    if (r.completeness != EvidenceCompleteness::ProvenUnderAssumptions) {
        out.reason = CandidateReason::IncompleteEvidence; return out;
    }
    if ((r.required_facts & r.proven_facts) != r.required_facts) {
        out.reason = CandidateReason::MissingProofObligation; return out;
    }
    if (r.exactness == CandidateExactness::Exact && r.validation != ValidationMethod::SemanticProof) {
        out.reason = CandidateReason::MissingSemanticProof; return out;
    }
    const bool verified_quality = r.validation == ValidationMethod::ScopedQualityTrial &&
        r.quality_accepted && !r.quality_scope.empty() && !r.quality_reference.empty();
    if (r.exactness == CandidateExactness::Approximate &&
        !verified_quality && !r.experimental_unverified) {
        out.reason = CandidateReason::MissingQualityEvidence; return out;
    }
    if (r.guard_boundary == GuardBoundary::None || !r.guard_before_effects) {
        out.reason = CandidateReason::MissingGuard; return out;
    }
    out.reason = CandidateReason::Accepted;
    out.projection = {r.id, r.analyzer_version, r.backend, r.kind, r.exactness,
                      r.guard_boundary, r.execution_lifetime, verified_quality,
                      r.fingerprint, r.required_guards, r.dependency_scope,
                      r.original_entry, r.variant_entry, {r.variant_parameters[0],
                      r.variant_parameters[1], r.variant_parameters[2], r.variant_parameters[3]}};
    return out;
}

class PublishedCandidate;
class CandidateLease final {
    PublishedCandidate* owner_{};
    const ExecutableProjection* projection_{};
    friend class PublishedCandidate;
    CandidateLease(PublishedCandidate* owner, const ExecutableProjection* projection) noexcept
        : owner_(owner), projection_(projection) {}
public:
    CandidateLease() = default;
    CandidateLease(const CandidateLease&) = delete;
    CandidateLease& operator=(const CandidateLease&) = delete;
    CandidateLease(CandidateLease&& other) noexcept;
    CandidateLease& operator=(CandidateLease&& other) noexcept;
    ~CandidateLease();
    [[nodiscard]] explicit operator bool() const noexcept { return projection_ != nullptr; }
    [[nodiscard]] const ExecutableProjection* get() const noexcept { return projection_; }
    [[nodiscard]] const ExecutableProjection* operator->() const noexcept { return projection_; }
    void reset() noexcept;
};

// The owner must outlive all leases. Analysis, eligibility, prepare, local
// validation and rejection have one control-thread owner. Invalidation may
// race activation, acquisition and release; the terminal gate bit prevents
// activation from reopening a candidate after invalidation. GPU fence/resource
// retention is external even when a submission-gating lease is released.
// try_acquire and lease release are allocation-free atomics.
class PublishedCandidate final {
    static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t invalidated = std::uint64_t{1} << 62;
    static constexpr std::uint64_t count_mask = invalidated - 1;
    std::atomic<std::uint64_t> gate_{closed};
    std::atomic<CandidateLifecycle> state_{CandidateLifecycle::Discovered};
    std::atomic<CandidateReason> reason_{CandidateReason::Accepted};
    std::atomic<std::uint64_t> diagnostic_loss_{0};
    ExecutableProjection projection_{};
    void finish_retirement() noexcept {
        if ((gate_.load(std::memory_order_acquire) & count_mask) == 0) {
            auto expected = CandidateLifecycle::Invalidated;
            state_.compare_exchange_strong(expected, CandidateLifecycle::Retired,
                                           std::memory_order_acq_rel);
        }
    }
    void release() noexcept {
        gate_.fetch_sub(1, std::memory_order_acq_rel);
        finish_retirement();
    }
    friend class CandidateLease;
public:
    PublishedCandidate() = default;
    PublishedCandidate(const PublishedCandidate&) = delete;
    PublishedCandidate& operator=(const PublishedCandidate&) = delete;
    ~PublishedCandidate() { if (outstanding() != 0) std::terminate(); }
    [[nodiscard]] CandidateLifecycle lifecycle() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] CandidateReason reason() const noexcept { return reason_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t outstanding() const noexcept { return gate_.load(std::memory_order_acquire) & count_mask; }
    [[nodiscard]] std::uint64_t diagnostic_loss() const noexcept { return diagnostic_loss_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool mark_analyzed() noexcept { return transition(CandidateLifecycle::Discovered, CandidateLifecycle::Analyzed); }
    [[nodiscard]] bool mark_eligible() noexcept { return transition(CandidateLifecycle::Analyzed, CandidateLifecycle::Eligible); }
    [[nodiscard]] bool prepare(const CandidateAdmission& admission) noexcept {
        if (!admission.accepted() || lifecycle() != CandidateLifecycle::Eligible) return false;
        projection_ = admission.projection;
        return transition(CandidateLifecycle::Eligible, CandidateLifecycle::Prepared);
    }
    [[nodiscard]] bool local_validate(bool passed) noexcept {
        if (!passed) { reject(CandidateReason::LocalValidationFailed); return false; }
        return transition(CandidateLifecycle::Prepared, CandidateLifecycle::LocallyValidated);
    }
    [[nodiscard]] bool activate() noexcept {
        if (!transition(CandidateLifecycle::LocallyValidated, CandidateLifecycle::Active)) return false;
        auto expected = closed;
        return gate_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    [[nodiscard]] CandidateLease try_acquire(const CandidateFingerprint& current,
                                             std::uint64_t supplied_guard_facts) noexcept {
        auto users = gate_.load(std::memory_order_acquire);
        while (!(users & closed) && (users & count_mask) < count_mask) {
            if (gate_.compare_exchange_weak(users, users + 1, std::memory_order_acq_rel)) {
                // Acquiring a lease makes the immutable projection safe to read,
                // even when invalidation races this guard check.
                if ((current == projection_.fingerprint) &&
                    (supplied_guard_facts & projection_.required_guards) == projection_.required_guards)
                    return CandidateLease(this, &projection_);
                release();
                return {};
            }
        }
        return {};
    }
    void invalidate(CandidateReason why) noexcept {
        gate_.fetch_or(closed | invalidated, std::memory_order_acq_rel); // closes admission first
        auto expected = lifecycle();
        while (expected <= CandidateLifecycle::Active) {
            if (state_.compare_exchange_weak(expected, CandidateLifecycle::Invalidated,
                                             std::memory_order_acq_rel)) {
                reason_.store(why, std::memory_order_release); break;
            }
        }
        finish_retirement();
    }
    void event_loss(std::uint64_t affected_scope, bool correctness_relevant) noexcept {
        if (!correctness_relevant) { diagnostic_loss_.fetch_add(1, std::memory_order_relaxed); return; }
        if (lifecycle() < CandidateLifecycle::Prepared) {
            invalidate(CandidateReason::CorrectnessEventLoss); return;
        }
        if (!projection_.dependency_scope || !affected_scope ||
            (projection_.dependency_scope & affected_scope))
            invalidate(CandidateReason::CorrectnessEventLoss);
    }
    void reject(CandidateReason why) noexcept {
        gate_.fetch_or(closed | invalidated, std::memory_order_acq_rel);
        auto current = lifecycle();
        while (current < CandidateLifecycle::Active) {
            const auto target = why == CandidateReason::BudgetExceeded
                ? CandidateLifecycle::BudgetExhausted : CandidateLifecycle::Rejected;
            if (state_.compare_exchange_weak(current, target, std::memory_order_acq_rel)) {
                reason_.store(why, std::memory_order_release); return;
            }
        }
        if (current == CandidateLifecycle::Active) invalidate(why);
    }
private:
    [[nodiscard]] bool transition(CandidateLifecycle from, CandidateLifecycle to) noexcept {
        return state_.compare_exchange_strong(from, to, std::memory_order_acq_rel);
    }
};

inline CandidateLease::CandidateLease(CandidateLease&& other) noexcept
    : owner_(other.owner_), projection_(other.projection_) { other.owner_ = nullptr; other.projection_ = nullptr; }
inline CandidateLease& CandidateLease::operator=(CandidateLease&& other) noexcept {
    if (this != &other) { reset(); owner_ = other.owner_; projection_ = other.projection_;
        other.owner_ = nullptr; other.projection_ = nullptr; }
    return *this;
}
inline CandidateLease::~CandidateLease() { reset(); }
inline void CandidateLease::reset() noexcept {
    if (owner_) owner_->release();
    owner_ = nullptr; projection_ = nullptr;
}

} // namespace arc
