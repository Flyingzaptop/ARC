#pragma once

#include "arc/runtime_coordinator.hpp"
#include "arc/runtime_event_bridge.hpp"

#include <cstdint>

namespace arc {

struct RuntimeIntegrationConfig {
    LiveRuntimeConfig runtime{};
    RuntimeCoordinatorConfig coordinator{};
};

// Thin host-facing facade. Emulator/native integrations can feed the same ARC
// observer events used by validation, provide real fence completions/budget
// samples, and explicitly opt controlled resources into mutation.
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

    bool consume(const Event& event) { return bridge_.consume(event); }
    void note_queue_completed(QueueId queue, std::uint64_t completed_fence) {
        bridge_.note_queue_completed(queue, completed_fence);
    }
    void update_budget(const MemoryBudgetPayload& budget) { runtime_.update_budget(budget); }

    [[nodiscard]] RuntimeTickResult tick();
    [[nodiscard]] RuntimeTickResult tick(std::uint64_t epoch);

    void set_mode(RuntimeMode mode) noexcept { coordinator_.set_mode(mode); }
    void set_backend(RuntimeMutationBackend* backend) noexcept { coordinator_.set_backend(backend); }
    void reset_circuit_breaker() noexcept { coordinator_.reset_circuit_breaker(); }

    [[nodiscard]] LiveRuntimeController& runtime() noexcept { return runtime_; }
    [[nodiscard]] const LiveRuntimeController& runtime() const noexcept { return runtime_; }
    [[nodiscard]] RuntimeEventBridge& bridge() noexcept { return bridge_; }
    [[nodiscard]] const RuntimeEventBridge& bridge() const noexcept { return bridge_; }
    [[nodiscard]] RuntimeCoordinator& coordinator() noexcept { return coordinator_; }
    [[nodiscard]] const RuntimeCoordinator& coordinator() const noexcept { return coordinator_; }

private:
    LiveRuntimeController runtime_;
    RuntimeEventBridge bridge_;
    RuntimeCoordinator coordinator_;
    std::uint64_t fallback_epoch_{};
};

}  // namespace arc
