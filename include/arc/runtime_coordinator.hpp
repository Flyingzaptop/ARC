#pragma once

#include "arc/live_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace arc {

enum class RuntimeMode : std::uint8_t {
    ObserveOnly,
    PlanOnly,
    Controlled,
};

enum class RuntimeBackendStatus : std::uint8_t {
    Success,
    Unsupported,
    UnsafeInFlight,
    OutOfBudget,
    Timeout,
    Failure,
};

enum class RuntimeTickStatus : std::uint8_t {
    NoAction,
    ObservedOnly,
    PlannedOnly,
    Executed,
    BudgetStale,
    ResolveFailed,
    BackendFailed,
    CommitFailed,
    CircuitOpen,
};

class RuntimeMutationBackend {
public:
    virtual ~RuntimeMutationBackend() = default;

    virtual RuntimeBackendStatus evict(ResourceId resource, const ResidencyAction& action) noexcept = 0;
    virtual RuntimeBackendStatus make_resident(ResourceId resource, const ResidencyAction& action) noexcept = 0;
    virtual RuntimeBackendStatus demote_texture(ResourceId resource, const TextureQualityAction& action) noexcept = 0;
    virtual RuntimeBackendStatus promote_texture(ResourceId resource, const TextureQualityAction& action) noexcept = 0;
};

struct RuntimeCoordinatorConfig {
    RuntimeMode mode{RuntimeMode::ObserveOnly};
    // Zero disables age checking. Otherwise a budget sample must have been
    // observed within this many coordinator ticks before mutations run. This
    // deliberately does not use resource-use epochs, which may advance many
    // thousands of times per rendered frame.
    std::uint64_t max_budget_age_ticks{120};
    std::uint32_t max_consecutive_failures{3};
    std::uint32_t max_actions_per_tick{64};
};

struct RuntimeCoordinatorMetrics {
    std::uint64_t ticks{};
    std::uint64_t observe_only_ticks{};
    std::uint64_t plan_only_ticks{};
    std::uint64_t controlled_ticks{};
    std::uint64_t stale_budget_blocks{};
    std::uint64_t resolve_failures{};
    std::uint64_t backend_failures{};
    std::uint64_t commit_failures{};
    std::uint64_t resolved_actions{};
    std::uint64_t executed_actions{};
    std::uint64_t rollback_attempts{};
    std::uint64_t rollback_failures{};
    std::uint64_t circuit_trips{};
};

struct RuntimeTickResult {
    RuntimeTickStatus status{RuntimeTickStatus::NoAction};
    RuntimeMode requested_mode{RuntimeMode::ObserveOnly};
    RuntimeMode effective_mode{RuntimeMode::ObserveOnly};
    bool budget_fresh{};
    bool circuit_open{};
    std::uint64_t budget_age_ticks{};
    std::size_t resolved_actions{};
    std::size_t executed_actions{};
    RuntimeBackendStatus backend_status{RuntimeBackendStatus::Success};
    LiveRuntimePlan plan{};
};

class RuntimeCoordinator final {
public:
    explicit RuntimeCoordinator(
        LiveRuntimeController& runtime,
        RuntimeMutationBackend* backend = nullptr,
        RuntimeCoordinatorConfig config = {});

    void set_backend(RuntimeMutationBackend* backend) noexcept { backend_ = backend; }
    void set_mode(RuntimeMode mode) noexcept { requested_mode_ = mode; }
    [[nodiscard]] RuntimeMode requested_mode() const noexcept { return requested_mode_; }
    [[nodiscard]] RuntimeMode effective_mode() const noexcept;

    void reset_circuit_breaker() noexcept;
    [[nodiscard]] bool circuit_open() const noexcept { return circuit_open_; }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return consecutive_failures_; }

    [[nodiscard]] RuntimeTickResult tick(std::uint64_t epoch);
    [[nodiscard]] RuntimeCoordinatorMetrics metrics() const noexcept { return metrics_; }
    [[nodiscard]] const RuntimeCoordinatorConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] bool refresh_budget_freshness(RuntimeTickResult& result) noexcept;
    [[nodiscard]] std::optional<std::vector<LiveRuntimeResolvedAction>> resolve(const LiveRuntimePlan& plan) const;
    [[nodiscard]] bool execute_action(
        const LiveRuntimeResolvedAction& action,
        std::uint64_t epoch,
        RuntimeTickResult& result);
    void record_failure(bool immediate_trip) noexcept;
    void trip_circuit() noexcept;

    LiveRuntimeController& runtime_;
    RuntimeMutationBackend* backend_{};
    RuntimeCoordinatorConfig config_{};
    RuntimeMode requested_mode_{RuntimeMode::ObserveOnly};
    std::uint64_t seen_budget_revision_{};
    std::uint64_t last_budget_tick_{};
    bool budget_seen_{};
    bool circuit_open_{};
    std::uint32_t consecutive_failures_{};
    RuntimeCoordinatorMetrics metrics_{};
};

}  // namespace arc
