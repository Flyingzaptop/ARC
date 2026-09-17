#pragma once

#include "arc/runtime_coordinator.hpp"
#include "arc/runtime_event_bridge.hpp"
#include "arc/unified_runtime_governor.hpp"

#include <cstdint>
#include <utility>

namespace arc {

struct RuntimeIntegrationConfig {
    LiveRuntimeConfig runtime{};
    RuntimeCoordinatorConfig coordinator{};
    UnifiedRuntimeGovernorConfig governor{};
};

// Host-facing facade. Emulator/native integrations can feed the same ARC
// observer events used by validation, provide real fence completions/budget
// samples, and explicitly opt controlled resources into mutation. Unlike the
// standalone bridge test helper, this production facade requires one explicit
// completion-fence identity per controlled queue.
class RuntimeIntegration final {
public:
    explicit RuntimeIntegration(
        RuntimeMutationBackend* backend = nullptr,
        RuntimeIntegrationConfig config = {});

    bool register_controlled_resource(ResidencyObject object) {
        return runtime_.register_controlled_resource(std::move(object));
    }
    bool register_controlled_texture(TextureQualityObject object) {
        return runtime_.register_controlled_texture(std::move(object));
    }
    bool unregister_resource(ResourceId resource) noexcept {
        return runtime_.unregister_resource(resource);
    }

    bool register_quality_profile(QualityResourceProfile profile) {
        return governor_.register_quality_profile(std::move(profile));
    }
    bool unregister_quality_profile(std::uint64_t id) noexcept {
        return governor_.unregister_quality_profile(id);
    }
    bool update_quality_importance(std::uint64_t id, ResourceImportanceSample importance) noexcept {
        return governor_.update_quality_importance(id, importance);
    }
    [[nodiscard]] QualityAdmissionDecision admit_quality(
        const QualityAdmissionResource& resource,
        const FrameBudgetSample& frame) {
        return governor_.admit(resource, frame);
    }

    bool consume(const Event& event) { return bridge_.consume(event); }
    bool bind_completion_fence(QueueId queue, std::uint64_t fence_id) noexcept {
        return bridge_.bind_completion_fence(queue, fence_id);
    }
    bool unbind_completion_fence(QueueId queue) noexcept {
        return bridge_.unbind_completion_fence(queue);
    }
    bool note_queue_completed(QueueId queue, std::uint64_t fence_id, std::uint64_t completed_fence) {
        return bridge_.note_queue_completed(queue, fence_id, completed_fence);
    }
    void update_budget(const MemoryBudgetPayload& budget) { runtime_.update_budget(budget); }

    [[nodiscard]] RuntimeTickResult tick();
    [[nodiscard]] RuntimeTickResult tick(std::uint64_t epoch);
    [[nodiscard]] UnifiedRuntimeTickResult tick_adaptive(const FrameBudgetSample& frame);
    [[nodiscard]] UnifiedRuntimeTickResult tick_adaptive(std::uint64_t epoch, const FrameBudgetSample& frame);

    void set_mode(RuntimeMode mode) noexcept { coordinator_.set_mode(mode); }
    void set_backend(RuntimeMutationBackend* backend) noexcept {
        coordinator_.set_backend(backend);
        governor_.set_backend(backend);
    }
    void reset_circuit_breaker() noexcept {
        coordinator_.reset_circuit_breaker();
        governor_.reset_quality_circuit();
    }

    [[nodiscard]] LiveRuntimeController& runtime() noexcept { return runtime_; }
    [[nodiscard]] const LiveRuntimeController& runtime() const noexcept { return runtime_; }
    [[nodiscard]] RuntimeEventBridge& bridge() noexcept { return bridge_; }
    [[nodiscard]] const RuntimeEventBridge& bridge() const noexcept { return bridge_; }
    [[nodiscard]] RuntimeCoordinator& coordinator() noexcept { return coordinator_; }
    [[nodiscard]] const RuntimeCoordinator& coordinator() const noexcept { return coordinator_; }
    [[nodiscard]] UnifiedRuntimeGovernor& governor() noexcept { return governor_; }
    [[nodiscard]] const UnifiedRuntimeGovernor& governor() const noexcept { return governor_; }

private:
    LiveRuntimeController runtime_;
    RuntimeEventBridge bridge_;
    RuntimeCoordinator coordinator_;
    UnifiedRuntimeGovernor governor_;
    std::uint64_t fallback_epoch_{};
};

}  // namespace arc
