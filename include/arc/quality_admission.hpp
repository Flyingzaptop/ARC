#pragma once

#include "arc/adaptive_quality.hpp"
#include "arc/quality_profile.hpp"

#include <cstdint>
#include <vector>

namespace arc {

struct QualityAdmissionResource {
    QualityResourceProfile profile{};
    std::uint64_t requested_bytes{};
    bool controllable{true};
};

struct QualityAdmissionConfig {
    // Admission remains conservative until either the GPU frame budget or the
    // local-memory budget is actually under pressure.
    double memory_pressure_enter{0.90};
    double memory_pressure_emergency{0.97};
    double frame_pressure_enter{1.03};
    double step1_threshold{0.22};
    double step2_threshold{0.48};
    double step3_threshold{0.72};
    bool protect_ui{true};
    bool protect_faces{true};
    bool protect_player_weapon{true};
};

struct QualityAdmissionDecision {
    bool valid{};
    bool reduced{};
    bool protected_semantic{};
    std::uint32_t admitted_level{};
    double importance_score{};
    double pressure_score{};
    double retained_quality{1.0};
    std::uint64_t estimated_bytes_saved{};
    std::vector<QualityActionCandidate> initial_actions{};
};

// Host-facing admission policy.  A renderer can call this before allocating a
// requested quality level.  ARC never changes the logical resource contract;
// the host decides how an admitted physical level maps to mips/LOD/pass quality.
class QualityAdmissionController final {
public:
    explicit QualityAdmissionController(QualityAdmissionConfig config = {});

    [[nodiscard]] QualityAdmissionDecision decide(
        const QualityAdmissionResource& resource,
        const FrameBudgetSample& frame) const;

    [[nodiscard]] const QualityAdmissionConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] bool semantic_protected(QualitySemanticClass semantic) const noexcept;

    QualityAdmissionConfig config_{};
};

} // namespace arc
