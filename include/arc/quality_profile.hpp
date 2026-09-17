#pragma once

#include "arc/adaptive_quality.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace arc {

enum class QualitySemanticClass : std::uint8_t {
    Generic,
    Ui,
    Face,
    PlayerWeapon,
    Character,
    Environment,
    Foliage,
    Effect,
};

struct QualityLevelStep {
    // Fraction of original quality retained after this step, [0,1].
    double retained_quality{1.0};
    double expected_ms_gain{};
    std::uint64_t memory_freed_bytes{};
};

struct QualityResourceProfile {
    std::uint64_t id{};
    QualityDomain domain{QualityDomain::Texture};
    std::string label{};
    QualitySemanticClass semantic{QualitySemanticClass::Generic};
    ResourceImportanceSample importance{};
    double confidence{1.0};
    bool reversible{true};
    bool temporal_assist{false};
    std::vector<QualityLevelStep> levels{};
};

class QualityCandidateFactory final {
public:
    [[nodiscard]] static std::vector<QualityActionCandidate> build(
        const QualityResourceProfile& profile);

    [[nodiscard]] static double semantic_protection(QualitySemanticClass semantic) noexcept;
};

} // namespace arc
