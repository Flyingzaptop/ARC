#pragma once
#include "arc/perceptual_trial.hpp"
#include <map>
#include <tuple>

namespace arc {
struct PredictivePerceptualConfig {
    std::size_t max_records{1024};
    std::uint64_t probe_cooldown_frames{120}, rejection_cooldown_frames{600};
    double restore_predicted_coverage{0.03}, minimum_visibility_confidence{0.75};
};
struct PerceptualLearningRecord {
    std::uint64_t action{},target{},generation{},accepted{},rejected{},cooldown_remaining{};
    double accepted_gain_ewma{};
    bool quarantined{};
};
struct PerceptualLearningCheckpoint {
    std::uint32_t version{1};
    std::uint64_t context{};
    std::vector<PerceptualLearningRecord> records;
};
enum class PredictiveDecision { Idle, Probed, Held, RestoredForVisibility, RestoredForUncertainty, Faulted };
struct PredictiveResult {
    PredictiveDecision decision{PredictiveDecision::Idle};
    PerceptualTrialResult trial;
};
// Session-local bounded calibration. Learning only affects proposal priority and
// cooldown; every new trial still runs the unchanged independent F critic.
class PredictivePerceptualOptimizer {
public:
    explicit PredictivePerceptualOptimizer(std::uint64_t context,
        PredictivePerceptualConfig prediction = {}, PerceptualGuardConfig guards = {});
    PredictiveResult step(PerceptualProbeHost&,std::uint64_t frame,
        std::span<const PerceptualCandidate>,std::span<const TemporalVisibilityState>);
    bool reset(PerceptualProbeHost&) noexcept;
    [[nodiscard]] bool active() const noexcept {return trials_.active();}
    [[nodiscard]] bool faulted() const noexcept {return trials_.faulted();}
    [[nodiscard]] std::size_t record_count()const noexcept {return records_.size();}
    [[nodiscard]] PerceptualLearningCheckpoint checkpoint() const;
    // Import only into an idle fresh session; context identifies device/driver/application.
    bool load(const PerceptualLearningCheckpoint&);
private:
    using Key=std::tuple<std::uint64_t,std::uint64_t,std::uint64_t>;
    struct Record {PerceptualLearningRecord learned;std::uint64_t eligible_frame{};};
    PerceptualTrialController trials_;
    PredictivePerceptualConfig config_;
    PerceptualGuardConfig guards_;
    std::uint64_t context_{},frame_{};
    std::map<Key,Record> records_;
    std::optional<PerceptualCandidate> active_candidate_;
    const TemporalVisibilityState* observation(std::span<const TemporalVisibilityState>,VisualTrackId,std::uint64_t) const noexcept;
    bool uncertain(const TemporalVisibilityState*)const noexcept;
    bool rising(const TemporalVisibilityState*)const noexcept;
};
} // namespace arc
