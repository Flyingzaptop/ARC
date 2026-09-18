#include "arc/dx12_host_adapter.hpp"

#include "arc/clock.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace arc::dx12 {

NativeHostAdapter::NativeHostAdapter(
    ID3D12Device* device,
    IDXGIAdapter3* adapter,
    NativeHostAdapterConfig config)
    : device_(device),
      adapter_(adapter),
      config_(std::move(config)),
      events_((std::max<std::size_t>)(64, config_.event_capacity)),
      observer_(events_, ids_, &global_sequence_),
      backend_(device),
      runtime_(&backend_, config_.runtime) {
    config_.default_reload_ms = (std::max)(0.0, config_.default_reload_ms);
}

QueueClass NativeHostAdapter::queue_class(const D3D12_COMMAND_LIST_TYPE type) noexcept {
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT: return QueueClass::Graphics;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return QueueClass::Compute;
    case D3D12_COMMAND_LIST_TYPE_COPY: return QueueClass::Copy;
    default: return QueueClass::Unknown;
    }
}

bool NativeHostAdapter::emit_locked(
    const EventType type,
    const void* payload,
    const std::uint32_t bytes) noexcept {
    if (payload == nullptr || bytes > kMaxEventPayloadBytes) return note_failure();
    Event event{};
    event.header.timestamp_ns = monotonic_time_ns();
    event.header.sequence = global_sequence_.fetch_add(1, std::memory_order_relaxed);
    event.header.thread_id = GetCurrentThreadId();
    event.header.type = type;
    event.header.flags = 1;
    event.header.payload_bytes = bytes;
    std::memcpy(event.payload.data(), payload, bytes);
    if (!events_.try_emit(event)) return note_failure();
    return true;
}

bool NativeHostAdapter::note_failure() noexcept {
    ++metrics_.failed_observations;
    return false;
}

ResourceId NativeHostAdapter::observe_committed_resource(ID3D12Resource* resource) {
    if (!resource || !device_) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = resources_.find(resource); it != resources_.end()) return it->second;
    const auto id = observer_.observe_committed_resource(device_.Get(), resource->GetDesc(), resource);
    if (!id) { note_failure(); return 0; }
    resources_.emplace(resource, id);
    ++metrics_.resources_observed;
    return id;
}

ResourceId NativeHostAdapter::observe_reserved_resource(ID3D12Resource* resource) {
    if (!resource || !device_) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = resources_.find(resource); it != resources_.end()) return it->second;
    const auto id = observer_.observe_reserved_resource(device_.Get(), resource->GetDesc(), resource);
    if (!id) { note_failure(); return 0; }
    resources_.emplace(resource, id);
    ++metrics_.resources_observed;
    return id;
}

ResourceId NativeHostAdapter::observe_placed_resource(
    ID3D12Resource* resource,
    ID3D12Heap* heap,
    const std::uint64_t heap_offset) {
    if (!resource || !heap || !device_) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = resources_.find(resource); it != resources_.end()) return it->second;
    HeapId hid{};
    if (const auto it = heaps_.find(heap); it != heaps_.end()) {
        hid = it->second;
    } else {
        const auto desc = heap->GetDesc();
        hid = observer_.observe_heap(desc, heap);
        if (!hid) { note_failure(); return 0; }
        heaps_.emplace(heap, hid);
        ++metrics_.heaps_observed;
    }
    const auto id = observer_.observe_placed_resource(device_.Get(), hid, heap_offset, resource->GetDesc(), resource);
    if (!id) { note_failure(); return 0; }
    resources_.emplace(resource, id);
    ++metrics_.resources_observed;
    return id;
}

bool NativeHostAdapter::observe_resource_destroyed(ID3D12Resource* resource) {
    if (!resource) return false;
    std::scoped_lock lock(mutex_);
    const auto it = resources_.find(resource);
    if (it == resources_.end()) return note_failure();
    const auto id = it->second;
    if (controlled_.erase(resource) != 0) {
        (void)backend_.unbind_resource(id);
        (void)runtime_.unregister_resource(id);
        if (metrics_.controlled_resources) --metrics_.controlled_resources;
    }
    observer_.observe_resource_destroyed(id);
    resources_.erase(it);
    ++metrics_.resources_destroyed;
    return true;
}

HeapId NativeHostAdapter::observe_heap(ID3D12Heap* heap, const D3D12_HEAP_DESC& description) {
    if (!heap) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = heaps_.find(heap); it != heaps_.end()) return it->second;
    const auto id = observer_.observe_heap(description, heap);
    if (!id) { note_failure(); return 0; }
    heaps_.emplace(heap, id);
    ++metrics_.heaps_observed;
    return id;
}

bool NativeHostAdapter::observe_heap_destroyed(ID3D12Heap* heap) {
    if (!heap) return false;
    std::scoped_lock lock(mutex_);
    const auto it = heaps_.find(heap);
    if (it == heaps_.end()) return note_failure();
    observer_.observe_heap_destroyed(it->second);
    heaps_.erase(it);
    return true;
}

std::uint64_t NativeHostAdapter::observe_descriptor_heap(ID3D12DescriptorHeap* heap) {
    if (!heap || !device_) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = descriptor_heaps_.find(heap); it != descriptor_heaps_.end()) return it->second.id;
    const auto desc = heap->GetDesc();
    const auto increment = device_->GetDescriptorHandleIncrementSize(desc.Type);
    const auto id = observer_.observe_descriptor_heap(desc, increment);
    if (!id) { note_failure(); return 0; }
    descriptor_heaps_.emplace(heap, DescriptorHeapRecord{id, desc.NumDescriptors, increment, {}});
    ++metrics_.descriptor_heaps_observed;
    return id;
}

bool NativeHostAdapter::observe_descriptor_heap_destroyed(ID3D12DescriptorHeap* heap) {
    if (!heap) return false;
    std::scoped_lock lock(mutex_);
    const auto it = descriptor_heaps_.find(heap);
    if (it == descriptor_heaps_.end()) return note_failure();
    observer_.observe_descriptor_heap_destroyed(it->second.id);
    descriptor_heaps_.erase(it);
    return true;
}

DescriptorId NativeHostAdapter::descriptor_id_locked(
    ID3D12DescriptorHeap* heap,
    const std::uint32_t index,
    const bool create) {
    const auto hit = descriptor_heaps_.find(heap);
    if (hit == descriptor_heaps_.end() || index >= hit->second.count) return 0;
    if (const auto it = hit->second.descriptors.find(index); it != hit->second.descriptors.end()) return it->second;
    if (!create) return 0;
    const auto id = ids_.next();
    const DescriptorLocationPayload payload{.descriptor = id, .heap = hit->second.id, .index = index};
    if (!emit_locked(EventType::DescriptorLocation, &payload, sizeof(payload))) return 0;
    hit->second.descriptors.emplace(index, id);
    return id;
}

DescriptorId NativeHostAdapter::descriptor_id(ID3D12DescriptorHeap* heap, const std::uint32_t index) const noexcept {
    if (!heap) return 0;
    std::scoped_lock lock(mutex_);
    const auto hit = descriptor_heaps_.find(heap);
    if (hit == descriptor_heaps_.end()) return 0;
    const auto it = hit->second.descriptors.find(index);
    return it == hit->second.descriptors.end() ? 0 : it->second;
}

bool NativeHostAdapter::observe_srv(
    ID3D12DescriptorHeap* heap, const std::uint32_t index,
    ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view) {
    if (!heap || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto rit = resources_.find(resource);
    if (rit == resources_.end()) return note_failure();
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_srv(id, rit->second, view)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

bool NativeHostAdapter::observe_uav(
    ID3D12DescriptorHeap* heap, const std::uint32_t index,
    ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& view) {
    if (!heap || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto rit = resources_.find(resource);
    if (rit == resources_.end()) return note_failure();
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_uav(id, rit->second, view)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

bool NativeHostAdapter::observe_rtv(
    ID3D12DescriptorHeap* heap, const std::uint32_t index,
    ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& view) {
    if (!heap || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto rit = resources_.find(resource);
    if (rit == resources_.end()) return note_failure();
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_rtv(id, rit->second, view)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

bool NativeHostAdapter::observe_dsv(
    ID3D12DescriptorHeap* heap, const std::uint32_t index,
    ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& view) {
    if (!heap || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto rit = resources_.find(resource);
    if (rit == resources_.end()) return note_failure();
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_dsv(id, rit->second, view)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

bool NativeHostAdapter::observe_sampler(ID3D12DescriptorHeap* heap, const std::uint32_t index) {
    if (!heap) return false;
    std::scoped_lock lock(mutex_);
    const auto id = descriptor_id_locked(heap, index, true);
    if (!id || !observer_.observe_sampler(id)) return note_failure();
    ++metrics_.descriptor_writes;
    return true;
}

QueueId NativeHostAdapter::observe_queue(ID3D12CommandQueue* queue, const D3D12_COMMAND_LIST_TYPE type) {
    if (!queue) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = queues_.find(queue); it != queues_.end()) return it->second;
    const auto id = ids_.next();
    const QueueCreatePayload payload{.queue = id, .type = queue_class(type)};
    if (!emit_locked(EventType::CommandQueueCreated, &payload, sizeof(payload))) return 0;
    queues_.emplace(queue, id);
    ++metrics_.queues_observed;
    return id;
}

CommandId NativeHostAdapter::observe_command_list(ID3D12CommandList* command, const D3D12_COMMAND_LIST_TYPE type) {
    if (!command) return 0;
    std::scoped_lock lock(mutex_);
    if (const auto it = commands_.find(command); it != commands_.end()) return it->second;
    const auto id = ids_.next();
    const CommandListPayload payload{.command = id, .type = queue_class(type)};
    if (!emit_locked(EventType::CommandListCreated, &payload, sizeof(payload))) return 0;
    commands_.emplace(command, id);
    ++metrics_.command_lists_observed;
    return id;
}

bool NativeHostAdapter::observe_command_list_reset(ID3D12CommandList* command) {
    if (!command) return false;
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(command);
    if (it == commands_.end()) return note_failure();
    const CommandListPayload payload{.command = it->second};
    return emit_locked(EventType::CommandListReset, &payload, sizeof(payload));
}

bool NativeHostAdapter::observe_command_list_closed(ID3D12CommandList* command) {
    if (!command) return false;
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(command);
    if (it == commands_.end()) return note_failure();
    const CommandListPayload payload{.command = it->second};
    return emit_locked(EventType::CommandListClosed, &payload, sizeof(payload));
}

bool NativeHostAdapter::observe_resource_use(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    const bool write) {
    if (!command || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto cit = commands_.find(command);
    const auto rit = resources_.find(resource);
    if (cit == commands_.end() || rit == resources_.end()) return note_failure();
    const ResourceUsePayload payload{.command = cit->second, .resource = rit->second, .write = write ? 1u : 0u};
    if (!emit_locked(EventType::ResourceUse, &payload, sizeof(payload))) return false;
    ++metrics_.resource_uses;
    return true;
}

bool NativeHostAdapter::observe_transition_barrier(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    const D3D12_RESOURCE_BARRIER& barrier) {
    if (!command || !resource) return false;
    std::scoped_lock lock(mutex_);
    const auto cit = commands_.find(command);
    const auto rit = resources_.find(resource);
    if (cit == commands_.end() || rit == resources_.end()) return note_failure();
    if (!observer_.observe_barrier(cit->second, rit->second, barrier)) return note_failure();
    return true;
}

bool NativeHostAdapter::observe_copy_resource(
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    const std::uint64_t approximate_bytes) {
    if (!command || !source || !destination) return false;
    std::scoped_lock lock(mutex_);
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
    return emit_locked(EventType::CopyResource, &payload, sizeof(payload));
}

bool NativeHostAdapter::observe_execute_command_lists(
    ID3D12CommandQueue* queue,
    const std::span<ID3D12CommandList* const> command_lists) {
    if (!queue || command_lists.empty()) return false;
    std::scoped_lock lock(mutex_);
    const auto qit = queues_.find(queue);
    if (qit == queues_.end()) return note_failure();
    bool ok = true;
    for (auto* command : command_lists) {
        const auto cit = commands_.find(command);
        if (cit == commands_.end()) { ok = false; note_failure(); continue; }
        const QueueSubmitPayload payload{
            .queue = qit->second,
            .command = cit->second,
            .submission = ++submission_sequence_,
        };
        if (!emit_locked(EventType::QueueSubmit, &payload, sizeof(payload))) ok = false;
        else ++metrics_.queue_submits;
    }
    return ok;
}

std::uint64_t NativeHostAdapter::bind_completion_fence(
    ID3D12CommandQueue* queue,
    ID3D12Fence* fence) {
    if (!queue || !fence) return 0;
    std::scoped_lock lock(mutex_);
    const auto qit = queues_.find(queue);
    if (qit == queues_.end()) { note_failure(); return 0; }
    if (const auto existing = completion_bindings_.find(queue); existing != completion_bindings_.end()) {
        return existing->second.fence.Get() == fence ? existing->second.fence_id : 0;
    }
    auto [fit, inserted] = fences_.emplace(fence, 0);
    if (inserted) fit->second = ids_.next();
    const auto fid = fit->second;
    if (!runtime_.bind_completion_fence(qit->second, fid) ||
        !backend_.bind_queue_fence(qit->second, fence)) {
        note_failure();
        return 0;
    }
    completion_bindings_.emplace(queue, CompletionBinding{qit->second, fid, fence});
    return fid;
}

bool NativeHostAdapter::unbind_completion_fence(ID3D12CommandQueue* queue) {
    if (!queue) return false;
    std::scoped_lock lock(mutex_);
    const auto it = completion_bindings_.find(queue);
    if (it == completion_bindings_.end()) return false;
    (void)runtime_.unbind_completion_fence(it->second.queue);
    (void)backend_.unbind_queue_fence(it->second.queue);
    completion_bindings_.erase(it);
    return true;
}

bool NativeHostAdapter::observe_fence_signal(
    ID3D12CommandQueue* queue,
    ID3D12Fence* fence,
    const std::uint64_t value) {
    if (!queue || !fence || !value) return false;
    std::scoped_lock lock(mutex_);
    const auto qit = queues_.find(queue);
    const auto fit = fences_.find(fence);
    if (qit == queues_.end() || fit == fences_.end()) return note_failure();
    const FencePayload payload{.queue = qit->second, .fence = fit->second, .value = value};
    if (!emit_locked(EventType::FenceSignal, &payload, sizeof(payload))) return false;
    ++metrics_.fence_signals;
    return true;
}

bool NativeHostAdapter::poll_completion(ID3D12CommandQueue* queue) {
    if (!queue) return false;
    std::scoped_lock lock(mutex_);
    const auto it = completion_bindings_.find(queue);
    if (it == completion_bindings_.end() || !it->second.fence) return false;
    const auto value = it->second.fence->GetCompletedValue();
    if (value == D3D12_FENCE_VALUE_MAX) return note_failure();
    if (!runtime_.note_queue_completed(it->second.queue, it->second.fence_id, value)) return note_failure();
    ++metrics_.completion_updates;
    return true;
}

std::uint64_t NativeHostAdapter::poll_all_completions() {
    std::scoped_lock lock(mutex_);
    std::uint64_t updates{};
    for (auto& [_, binding] : completion_bindings_) {
        if (!binding.fence) continue;
        const auto value = binding.fence->GetCompletedValue();
        if (value == D3D12_FENCE_VALUE_MAX) { note_failure(); continue; }
        if (runtime_.note_queue_completed(binding.queue, binding.fence_id, value)) {
            ++updates;
            ++metrics_.completion_updates;
        } else {
            note_failure();
        }
    }
    return updates;
}

bool NativeHostAdapter::observe_present(
    const std::uint64_t swapchain_id,
    const std::uint32_t sync_interval,
    const std::uint32_t flags,
    const HRESULT result) {
    std::scoped_lock lock(mutex_);
    const PresentPayload payload{
        .swapchain = swapchain_id,
        .frame = ++frame_,
        .sync_interval = sync_interval,
        .flags = flags,
        .result = static_cast<std::int32_t>(result),
    };
    if (!emit_locked(EventType::Present, &payload, sizeof(payload))) return false;
    ++metrics_.presents;
    return true;
}

bool NativeHostAdapter::sample_memory_budget() {
    if (!adapter_) return false;
    const auto budget = query_memory_budget(adapter_.Get());
    if (!budget) return false;
    std::scoped_lock lock(mutex_);
    if (!emit_locked(EventType::MemoryBudgetSample, &*budget, sizeof(*budget))) return false;
    ++metrics_.budget_samples;
    return true;
}

std::size_t NativeHostAdapter::drain_locked() {
    std::size_t count{};
    Event event{};
    while (events_.try_pop(event)) {
        graph_.consume(event);
        if (!runtime_.consume(event)) ++metrics_.bridge_rejections;
        ++count;
    }
    metrics_.events_drained += count;
    return count;
}

std::size_t NativeHostAdapter::drain() {
    std::scoped_lock lock(mutex_);
    return drain_locked();
}

bool NativeHostAdapter::enable_residency_control(
    ID3D12Resource* resource,
    double reload_ms,
    const ResidencySafety safety) {
    if (!resource || safety == ResidencySafety::Unknown) return false;
    std::scoped_lock lock(mutex_);
    const auto it = resources_.find(resource);
    if (it == resources_.end()) return note_failure();
    if (controlled_.contains(resource)) return true;
    (void)drain_locked();
    const auto record = graph_.find(it->second);
    if (!record || !record->alive) return note_failure();
    std::uint64_t bytes = record->description.allocation_bytes;
    if (!bytes && device_) {
        const auto desc = resource->GetDesc();
        bytes = device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }
    if (!bytes) return note_failure();
    if (reload_ms < 0.0) reload_ms = config_.default_reload_ms;
    reload_ms = (std::max)(0.0, reload_ms);

    if (!backend_.bind_resource(it->second, resource)) return note_failure();
    ResidencyObject object{};
    object.id = it->second;
    object.resource = it->second;
    object.state = safety == ResidencySafety::Pinned ? ResidencyState::Pinned : ResidencyState::Resident;
    object.safety = safety;
    object.cost.bytes = bytes;
    object.cost.reload_ms = reload_ms;
    object.last_resident_epoch = (std::max<std::uint64_t>)(1, runtime_.bridge().logical_epoch());
    if (!runtime_.register_controlled_resource(object)) {
        (void)backend_.unbind_resource(it->second);
        return note_failure();
    }
    controlled_.emplace(resource, true);
    ++metrics_.controlled_resources;
    return true;
}

bool NativeHostAdapter::disable_control(ID3D12Resource* resource) {
    if (!resource) return false;
    std::scoped_lock lock(mutex_);
    const auto it = resources_.find(resource);
    if (it == resources_.end() || !controlled_.contains(resource)) return false;
    const bool runtime_ok = runtime_.unregister_resource(it->second);
    const bool backend_ok = backend_.unbind_resource(it->second);
    controlled_.erase(resource);
    if (metrics_.controlled_resources) --metrics_.controlled_resources;
    return runtime_ok && backend_ok;
}

UnifiedRuntimeTickResult NativeHostAdapter::frame_tick(
    FrameBudgetSample frame,
    const bool query_budget) {
    if (query_budget) (void)sample_memory_budget();
    (void)poll_all_completions();
    (void)drain();
    graph_.analyze();
    if (const auto budget = graph_.latest_budget()) {
        if (!frame.local_budget_bytes) frame.local_budget_bytes = budget->local_budget;
        if (!frame.local_usage_bytes) frame.local_usage_bytes = budget->local_usage;
    }
    return runtime_.tick_adaptive(frame);
}

ResourceId NativeHostAdapter::resource_id(ID3D12Resource* resource) const noexcept {
    if (!resource) return 0;
    std::scoped_lock lock(mutex_);
    const auto it = resources_.find(resource);
    return it == resources_.end() ? 0 : it->second;
}

HeapId NativeHostAdapter::heap_id(ID3D12Heap* heap) const noexcept {
    if (!heap) return 0;
    std::scoped_lock lock(mutex_);
    const auto it = heaps_.find(heap);
    return it == heaps_.end() ? 0 : it->second;
}

QueueId NativeHostAdapter::queue_id(ID3D12CommandQueue* queue) const noexcept {
    if (!queue) return 0;
    std::scoped_lock lock(mutex_);
    const auto it = queues_.find(queue);
    return it == queues_.end() ? 0 : it->second;
}

CommandId NativeHostAdapter::command_id(ID3D12CommandList* command) const noexcept {
    if (!command) return 0;
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(command);
    return it == commands_.end() ? 0 : it->second;
}

std::uint64_t NativeHostAdapter::fence_id(ID3D12Fence* fence) const noexcept {
    if (!fence) return 0;
    std::scoped_lock lock(mutex_);
    const auto it = fences_.find(fence);
    return it == fences_.end() ? 0 : it->second;
}

NativeHostAdapterMetrics NativeHostAdapter::metrics() const noexcept {
    std::scoped_lock lock(mutex_);
    return metrics_;
}

std::vector<ResourceSemanticEstimate> NativeHostAdapter::semantic_snapshot() const {
    std::scoped_lock lock(mutex_);
    return SceneUnderstandingModel::infer_all(graph_);
}

CompatibilityDecision NativeHostAdapter::compatibility_snapshot() const {
    std::scoped_lock lock(mutex_);
    const auto semantics = SceneUnderstandingModel::infer_all(graph_);
    const auto bridge = runtime_.bridge().metrics();
    const auto governor = runtime_.governor().metrics();
    CompatibilityTelemetry telemetry{};
    telemetry.observation_failures = metrics_.failed_observations;
    telemetry.bridge_rejections = metrics_.bridge_rejections + bridge.controller_rejections;
    telemetry.malformed_events = bridge.malformed_events;
    telemetry.backend_failures = governor.quality_backend_failures;
    return CompatibilityGuard::evaluate(graph_, semantics, telemetry);
}

}  // namespace arc::dx12
