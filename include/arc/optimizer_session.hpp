#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace arc {
struct OptimizerSessionConfig {
    double target_fps{60};
    double min_ssim{.98},max_mean_error{.01},max_tile_p99{.04};
    double min_gain_ms{.1},min_gain_fraction{.02};
    double max_cpu_overhead_ms{.2},max_gpu_overhead_ms{.25};
    std::uint32_t warmup_samples{32},settle_samples{8},hold_samples{120};
    // A successful image trial is not a permanent quality certificate. Return
    // to the original and acquire fresh evidence even while meeting the target.
    std::uint32_t max_evidence_samples{600};
};
struct OptimizerTrialEvidence {
    std::uint64_t action{},generation{};
    bool matched_reference{},complete{},restoration_confirmed{};
    double baseline_frame_ms{},candidate_frame_ms{},baseline_noise_ms{};
    double ssim{},mean_error{},tile_p99{},cpu_overhead_ms{},gpu_overhead_ms{};
};
enum class SessionPhase { Warmup,Discover,Probe,Settle,Active,Limited,Recover,Faulted };
enum class SessionRequestKind { None,Profile,Probe,Apply,Restore };
struct SessionRequest {SessionRequestKind kind{};std::uint64_t action{};};
struct SessionAction {
    // A complete reversible configuration, not an unvalidated incremental
    // mutation. Trials compare its final image against the original reference.
    std::uint64_t id{},generation{};
    double predicted_gain_ms{},quality_cost{};
    bool ready{};
};
struct SessionSnapshot {
    SessionPhase phase{SessionPhase::Warmup};
    double target_frame_ms{},filtered_frame_ms{};
    std::uint64_t active_action{},probes{},accepted{},rejected{},restores{};
};
// One serial decision/validation transaction. Backend ownership, same-input or
// confidence-qualified reference acquisition and GPU restoration are explicit
// contracts; the policy cannot manufacture quality evidence from FPS alone.
class OptimizerSession {
public:
    explicit OptimizerSession(OptimizerSessionConfig={});
    void target(double fps);
    void candidates(std::vector<SessionAction>);
    SessionRequest frame(double frame_ms,bool scene_stable=true);
    SessionRequest evidence(const OptimizerTrialEvidence&);
    void applied(std::uint64_t action,bool success);
    void restored(bool success);
    SessionRequest scene_changed();
    [[nodiscard]] SessionSnapshot snapshot()const noexcept{return state_;}
private:
    OptimizerSessionConfig config_;
    SessionSnapshot state_;
    std::vector<SessionAction> actions_;
    std::vector<std::uint64_t> tried_;
    std::optional<SessionAction> pending_;
    std::uint32_t samples_{},settle_{},hold_{};
    bool profile_requested_{},apply_pending_{},restore_pending_{};
    bool candidate_epoch_changed_{};
    std::uint64_t active_generation_{};
    std::uint32_t evidence_age_{};
    double retained_gain_ms_{};
};
}
