#pragma once
#include "arc/ids.hpp"
#include "arc/events.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
namespace arc {
using ResidencyId = std::uint64_t;
enum class ResidencyState : std::uint8_t { Unknown, Resident, Evicted, PendingResident, Pinned };
enum class ResidencySafety : std::uint8_t { Unknown, ControlledSafe, Pinned };
enum class PressureState : std::uint8_t { Normal, Pressure, Emergency };
struct ResidencyCost { std::uint64_t bytes{}; double reload_ms{}, reuse_interval{}; };
struct ResidencyPolicyConfig {
    double pressure_enter{0.85};
    double emergency_enter{0.95};
    double pressure_exit{0.80};
    double emergency_exit{0.85};
    double pressure_target{0.78};
    double emergency_target{0.75};
    double promotion_ceiling{0.78};
    std::uint64_t minimum_headroom_bytes{};
    std::uint64_t minimum_residency_age_epochs{8};
    std::uint64_t prefetch_horizon_epochs{8};
    std::uint32_t recovery_samples{3};
};
struct ResidencyObject {
    ResidencyId id{}; ResourceId resource{}; ResidencyState state{ResidencyState::Unknown};
    ResidencySafety safety{ResidencySafety::Unknown}; ResidencyCost cost{};
    std::uint64_t last_use_epoch{}, last_use_fence{}, last_completed_fence{}, use_count{};
    QueueId last_use_queue{};
};
struct ResidencyAction {
    enum class Type : std::uint8_t { Evict, MakeResident } type{};
    ResidencyId object{}; std::uint64_t bytes{}; std::uint64_t predicted_use_epoch{}; double score{};
    QueueId required_queue{}; std::uint64_t required_fence{};
    friend bool operator==(const ResidencyAction&, const ResidencyAction&) = default;
};
struct ResidencyTransitionPayload { ResidencyId object{}; ResidencyState before{}, after{}; std::uint8_t reserved[6]{}; std::uint64_t fence_value{}, bytes{}; };
static_assert(sizeof(ResidencyTransitionPayload) <= kMaxEventPayloadBytes);
struct ResidencyMetrics { std::uint64_t useful_evictions{}, false_evictions{}, reloads{}, bytes_evicted{}, bytes_made_resident{}, late_residency{}; };
struct ResidencyPlanSummary {
    PressureState pressure{PressureState::Normal};
    std::uint64_t budget{}, usage{}, target_usage{}, bytes_to_free{}, bytes_planned_to_free{}, bytes_planned_to_promote{};
    bool shortfall{};
};
class ResidencyGovernor {
public:
    explicit ResidencyGovernor(ResidencyPolicyConfig config = {});
    bool register_object(ResidencyObject object);
    bool note_use(ResidencyId id, std::uint64_t epoch, QueueId queue, std::uint64_t submitted_fence, std::uint64_t completed_fence);
    bool note_use(ResidencyId id, std::uint64_t epoch, std::uint64_t submitted_fence, std::uint64_t completed_fence) { return note_use(id, epoch, 0, submitted_fence, completed_fence); }
    bool note_use(ResidencyId id, std::uint64_t epoch, std::uint64_t completed_fence) { return note_use(id, epoch, 0, completed_fence, completed_fence); }
    bool note_completed(ResidencyId id, std::uint64_t completed_fence);
    bool transition(ResidencyId id, ResidencyState expected, ResidencyState next);
    void update_budget(std::uint64_t budget, std::uint64_t usage);
    [[nodiscard]] std::uint64_t bytes_to_free() const noexcept;
    [[nodiscard]] std::vector<ResidencyAction> plan_evictions(std::uint64_t epoch) const;
    [[nodiscard]] std::vector<ResidencyAction> plan_promotions(std::uint64_t epoch) const;
    [[nodiscard]] std::optional<ResidencyAction> require_resident(ResidencyId id) const;
    [[nodiscard]] std::vector<ResidencyAction> plan(std::uint64_t epoch) const;
    [[nodiscard]] ResidencyPlanSummary plan_summary(std::uint64_t epoch) const;
    [[nodiscard]] std::optional<ResidencyObject> find(ResidencyId id) const;
    [[nodiscard]] std::optional<std::uint64_t> predicted_next_use(ResidencyId id, std::uint64_t epoch) const;
    [[nodiscard]] PressureState pressure() const noexcept { return pressure_; }
    [[nodiscard]] bool speculative_promotions_allowed() const noexcept { return pressure_ == PressureState::Normal; }
    [[nodiscard]] ResidencyMetrics metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::uint64_t budget() const noexcept { return budget_; }
    [[nodiscard]] std::uint64_t usage() const noexcept { return usage_; }
    [[nodiscard]] const ResidencyPolicyConfig& config() const noexcept { return config_; }
    void record_eviction(ResidencyId id, bool later_reloaded);
    void record_resident(ResidencyId id, bool late);
private:
    [[nodiscard]] std::optional<std::uint64_t> predicted_next_use(const ResidencyObject& object, std::uint64_t epoch) const;
    [[nodiscard]] bool eviction_fence_safe(const ResidencyObject& object) const noexcept;
    [[nodiscard]] std::uint64_t target_usage_bytes() const noexcept;
    std::unordered_map<ResidencyId, ResidencyObject> objects_;
    ResidencyPolicyConfig config_{};
    PressureState pressure_{PressureState::Normal};
    ResidencyMetrics metrics_{};
    std::uint64_t budget_{}, usage_{};
    std::uint32_t stable_samples_{};
};
}
