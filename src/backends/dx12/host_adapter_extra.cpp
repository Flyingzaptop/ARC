#include "arc/dx12_host_adapter.hpp"

namespace arc::dx12 {

ResourceId NativeHostAdapter::observe_external_resource(ID3D12Resource* resource) {
    if (!resource || !device_) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = resources_.find(resource); it != resources_.end()) return it->second;
    const auto id = observer_.observe_external_resource(device_.Get(), resource);
    if (!id) { note_failure(); return 0; }
    resources_.emplace(resource, id);
    ++metrics_.resources_observed;
    return id;
}

bool NativeHostAdapter::observe_command_counters(
    ID3D12CommandList* command,
    const std::uint64_t draws,
    const std::uint64_t indexed_draws,
    const std::uint64_t dispatches,
    const std::uint64_t indirect) {
    if (!command) return false;
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(command);
    if (it == commands_.end()) return note_failure();
    const CountersPayload payload{
        .command = it->second,
        .draws = draws,
        .indexed_draws = indexed_draws,
        .dispatches = dispatches,
        .indirect = indirect,
    };
    return emit_locked(EventType::CommandCounters, &payload, sizeof(payload));
}

bool NativeHostAdapter::observe_memory_budget(const MemoryBudgetPayload& budget) {
    if (!budget.local_budget && !budget.nonlocal_budget) return false;
    std::scoped_lock lock(mutex_);
    if (!emit_locked(EventType::MemoryBudgetSample, &budget, sizeof(budget))) return false;
    ++metrics_.budget_samples;
    return true;
}

} // namespace arc::dx12
