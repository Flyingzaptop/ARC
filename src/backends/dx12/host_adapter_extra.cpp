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

bool NativeHostAdapter::observe_cbv(
    ID3D12DescriptorHeap* heap,
    const std::uint32_t index,
    ID3D12Resource* resource,
    const std::uint64_t resource_offset,
    const std::uint32_t bytes) {
    if (!heap || !resource || !bytes) return false;
    std::scoped_lock lock(mutex_);
    const auto rit = resources_.find(resource);
    if (rit == resources_.end()) return note_failure();
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_cbv(id, rit->second, resource_offset, bytes)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

bool NativeHostAdapter::observe_descriptor_copy(
    ID3D12DescriptorHeap* source_heap,
    const std::uint32_t source_index,
    ID3D12DescriptorHeap* destination_heap,
    const std::uint32_t destination_index) {
    if (!source_heap || !destination_heap) return false;
    std::scoped_lock lock(mutex_);
    const auto source = descriptor_id_locked(source_heap, source_index, false);
    const auto destination = descriptor_id_locked(destination_heap, destination_index, true);
    if (!source || !destination) return note_failure();
    const DescriptorCopyPayload payload{.source = source, .destination = destination};
    if (!emit_locked(EventType::DescriptorCopied, &payload, sizeof(payload))) return false;
    ++metrics_.descriptor_copies;
    return true;
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
    if (!emit_locked(EventType::CommandCounters, &payload, sizeof(payload))) return false;
    metrics_.draws_observed += draws;
    metrics_.indexed_draws_observed += indexed_draws;
    metrics_.dispatches_observed += dispatches;
    metrics_.indirect_observed += indirect;
    return true;
}

bool NativeHostAdapter::observe_global_barrier(
    ID3D12CommandList* command,
    const D3D12_GLOBAL_BARRIER& barrier) {
    if (!command) return false;
    std::scoped_lock lock(mutex_);
    const auto cit = commands_.find(command);
    if (cit == commands_.end()) return note_failure();
    if (!observer_.observe_barrier(cit->second, barrier)) return note_failure();
    ++metrics_.barriers_observed;
    return true;
}

bool NativeHostAdapter::observe_buffer_barrier(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    const D3D12_BUFFER_BARRIER& barrier) {
    if (!command || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto cit = commands_.find(command);
    const auto rit = resources_.find(resource);
    if (cit == commands_.end() || rit == resources_.end()) return note_failure();
    if (!observer_.observe_barrier(cit->second, rit->second, barrier)) return note_failure();
    ++metrics_.barriers_observed;
    return true;
}

bool NativeHostAdapter::observe_texture_barrier(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    const D3D12_TEXTURE_BARRIER& barrier) {
    if (!command || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto cit = commands_.find(command);
    const auto rit = resources_.find(resource);
    if (cit == commands_.end() || rit == resources_.end()) return note_failure();
    if (!observer_.observe_barrier(cit->second, rit->second, barrier)) return note_failure();
    ++metrics_.barriers_observed;
    return true;
}

bool NativeHostAdapter::observe_copy_locked(
    const EventType type,
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    const std::uint64_t approximate_bytes) {
    const auto cit = commands_.find(command);
    const auto sit = resources_.find(source);
    const auto dit = resources_.find(destination);
    if (cit == commands_.end() || sit == resources_.end() || dit == resources_.end()) return note_failure();
    const CopyPayload payload{
        .source = sit->second,
        .destination = dit->second,
        .command = cit->second,
        .approximate_bytes = approximate_bytes,
    };
    if (!emit_locked(type, &payload, sizeof(payload))) return false;
    ++metrics_.copies_observed;
    return true;
}

bool NativeHostAdapter::observe_copy_buffer(
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    const std::uint64_t approximate_bytes) {
    if (!command || !source || !destination) return false;
    std::scoped_lock lock(mutex_);
    return observe_copy_locked(EventType::CopyBuffer, command, source, destination, approximate_bytes);
}

bool NativeHostAdapter::observe_copy_texture(
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    const std::uint64_t approximate_bytes) {
    if (!command || !source || !destination) return false;
    std::scoped_lock lock(mutex_);
    return observe_copy_locked(EventType::CopyTexture, command, source, destination, approximate_bytes);
}

bool NativeHostAdapter::observe_memory_budget(const MemoryBudgetPayload& budget) {
    if (!budget.local_budget && !budget.nonlocal_budget) return false;
    std::scoped_lock lock(mutex_);
    if (!emit_locked(EventType::MemoryBudgetSample, &budget, sizeof(budget))) return false;
    ++metrics_.budget_samples;
    return true;
}

} // namespace arc::dx12
