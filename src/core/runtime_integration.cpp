#include "arc/runtime_integration.hpp"

#include <algorithm>
#include <limits>

namespace arc {

RuntimeIntegration::RuntimeIntegration(RuntimeMutationBackend* backend, RuntimeIntegrationConfig config)
    : runtime_(config.runtime),
      bridge_(runtime_, true),
      coordinator_(runtime_, backend, config.coordinator),
      governor_(runtime_, coordinator_, backend, config.governor) {}

RuntimeTickResult RuntimeIntegration::tick() {
    if (fallback_epoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++fallback_epoch_;
    const auto bridge_epoch = bridge_.logical_epoch();
    const auto epoch = (std::max)(fallback_epoch_, bridge_epoch ? bridge_epoch : std::uint64_t{1});
    fallback_epoch_ = epoch;
    return coordinator_.tick(epoch);
}

RuntimeTickResult RuntimeIntegration::tick(std::uint64_t epoch) {
    if (!epoch) return tick();
    if (epoch < fallback_epoch_) epoch = fallback_epoch_;
    fallback_epoch_ = epoch;
    return coordinator_.tick(epoch);
}

UnifiedRuntimeTickResult RuntimeIntegration::tick_adaptive(const FrameBudgetSample& frame) {
    if (fallback_epoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++fallback_epoch_;
    const auto bridge_epoch = bridge_.logical_epoch();
    const auto epoch = (std::max)(fallback_epoch_, bridge_epoch ? bridge_epoch : std::uint64_t{1});
    fallback_epoch_ = epoch;
    return governor_.tick(epoch, frame);
}

UnifiedRuntimeTickResult RuntimeIntegration::tick_adaptive(
    std::uint64_t epoch,
    const FrameBudgetSample& frame) {
    if (!epoch) return tick_adaptive(frame);
    if (epoch < fallback_epoch_) epoch = fallback_epoch_;
    fallback_epoch_ = epoch;
    return governor_.tick(epoch, frame);
}

}  // namespace arc
