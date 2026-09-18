#pragma once

#include "arc/dx12_observer.hpp"
#include "arc/dx12_runtime_backend.hpp"
#include "arc/compatibility_guard.hpp"
#include "arc/runtime_integration.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arc::dx12 {

struct NativeHostAdapterConfig {
    std::size_t event_capacity{16384};
    RuntimeIntegrationConfig runtime{};
    double default_reload_ms{0.25};
    bool enforce_compatibility_guard{false};
};

struct NativeHostAdapterMetrics {
    std::uint64_t resources_observed{};
    std::uint64_t resources_destroyed{};
    std::uint64_t heaps_observed{};
    std::uint64_t descriptor_heaps_observed{};
    std::uint64_t descriptor_writes{};
    std::uint64_t descriptor_copies{};
    std::uint64_t queues_observed{};
    std::uint64_t command_lists_observed{};
    std::uint64_t resource_uses{};
    std::uint64_t draws_observed{};
    std::uint64_t indexed_draws_observed{};
    std::uint64_t dispatches_observed{};
    std::uint64_t indirect_observed{};
    std::uint64_t barriers_observed{};
    std::uint64_t copies_observed{};
    std::uint64_t queue_submits{};
    std::uint64_t fence_signals{};
    std::uint64_t completion_updates{};
    std::uint64_t presents{};
    std::uint64_t budget_samples{};
    std::uint64_t events_drained{};
    std::uint64_t bridge_rejections{};
    std::uint64_t controlled_resources{};
    std::uint64_t compatibility_blocks{};
    std::uint64_t failed_observations{};
};

// Cooperative/native D3D12 integration boundary for engines and renderers that
// already own their device, resources and command submission. This class does
// not hook COM vtables, inject DLLs, replace descriptors, or infer mutation
// safety. The host reports successful D3D12 operations and explicitly opts
// resources into ARC control.
class NativeHostAdapter final {
public:
    NativeHostAdapter(
        ID3D12Device* device,
        IDXGIAdapter3* adapter,
        NativeHostAdapterConfig config = {});

    NativeHostAdapter(const NativeHostAdapter&) = delete;
    NativeHostAdapter& operator=(const NativeHostAdapter&) = delete;

    [[nodiscard]] ResourceId observe_committed_resource(ID3D12Resource* resource);
    [[nodiscard]] ResourceId observe_external_resource(ID3D12Resource* resource);
    [[nodiscard]] ResourceId observe_reserved_resource(ID3D12Resource* resource);
    [[nodiscard]] ResourceId observe_placed_resource(
        ID3D12Resource* resource,
        ID3D12Heap* heap,
        std::uint64_t heap_offset);
    bool observe_resource_destroyed(ID3D12Resource* resource);

    [[nodiscard]] HeapId observe_heap(ID3D12Heap* heap, const D3D12_HEAP_DESC& description);
    bool observe_heap_destroyed(ID3D12Heap* heap);

    [[nodiscard]] std::uint64_t observe_descriptor_heap(ID3D12DescriptorHeap* heap);
    bool observe_descriptor_heap_destroyed(ID3D12DescriptorHeap* heap);
    [[nodiscard]] DescriptorId descriptor_id(ID3D12DescriptorHeap* heap, std::uint32_t index) const noexcept;
    bool observe_cbv(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        std::uint64_t resource_offset,
        std::uint32_t bytes);
    bool observe_srv(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& view);
    bool observe_uav(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC& view);
    bool observe_rtv(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_RENDER_TARGET_VIEW_DESC& view);
    bool observe_dsv(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC& view);
    bool observe_sampler(ID3D12DescriptorHeap* heap, std::uint32_t index);
    bool observe_descriptor_copy(
        ID3D12DescriptorHeap* source_heap,
        std::uint32_t source_index,
        ID3D12DescriptorHeap* destination_heap,
        std::uint32_t destination_index);

    [[nodiscard]] QueueId observe_queue(ID3D12CommandQueue* queue, D3D12_COMMAND_LIST_TYPE type);
    [[nodiscard]] CommandId observe_command_list(ID3D12CommandList* command, D3D12_COMMAND_LIST_TYPE type);
    bool observe_command_list_reset(ID3D12CommandList* command);
    bool observe_command_list_closed(ID3D12CommandList* command);
    bool observe_resource_use(ID3D12CommandList* command, ID3D12Resource* resource, bool write = false);
    bool observe_command_counters(
        ID3D12CommandList* command,
        std::uint64_t draws,
        std::uint64_t indexed_draws,
        std::uint64_t dispatches,
        std::uint64_t indirect);
    bool observe_transition_barrier(
        ID3D12CommandList* command,
        ID3D12Resource* resource,
        const D3D12_RESOURCE_BARRIER& barrier);
    bool observe_global_barrier(
        ID3D12CommandList* command,
        const D3D12_GLOBAL_BARRIER& barrier);
    bool observe_buffer_barrier(
        ID3D12CommandList* command,
        ID3D12Resource* resource,
        const D3D12_BUFFER_BARRIER& barrier);
    bool observe_texture_barrier(
        ID3D12CommandList* command,
        ID3D12Resource* resource,
        const D3D12_TEXTURE_BARRIER& barrier);
    bool observe_copy_resource(
        ID3D12CommandList* command,
        ID3D12Resource* source,
        ID3D12Resource* destination,
        std::uint64_t approximate_bytes = 0);
    bool observe_copy_buffer(
        ID3D12CommandList* command,
        ID3D12Resource* source,
        ID3D12Resource* destination,
        std::uint64_t approximate_bytes = 0);
    bool observe_copy_texture(
        ID3D12CommandList* command,
        ID3D12Resource* source,
        ID3D12Resource* destination,
        std::uint64_t approximate_bytes = 0);
    bool observe_execute_command_lists(
        ID3D12CommandQueue* queue,
        std::span<ID3D12CommandList* const> command_lists);

    // ARC requires one monotonic completion fence per controlled queue. The
    // host may use its own fence; ARC only observes signal/completion values.
    [[nodiscard]] std::uint64_t bind_completion_fence(ID3D12CommandQueue* queue, ID3D12Fence* fence);
    bool unbind_completion_fence(ID3D12CommandQueue* queue);
    bool observe_fence_signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value);
    bool poll_completion(ID3D12CommandQueue* queue);
    std::uint64_t poll_all_completions();

    bool observe_present(std::uint64_t swapchain_id, std::uint32_t sync_interval, std::uint32_t flags, HRESULT result);
    bool sample_memory_budget();
    bool observe_memory_budget(const MemoryBudgetPayload& budget);

    // Mutation is opt-in. Observed resources remain read-only until the host
    // explicitly marks them safe for residency control.
    bool enable_residency_control(
        ID3D12Resource* resource,
        double reload_ms = -1.0,
        ResidencySafety safety = ResidencySafety::ControlledSafe);
    bool disable_control(ID3D12Resource* resource);

    bool register_quality_profile(QualityResourceProfile profile) {
        return runtime_.register_quality_profile(std::move(profile));
    }
    bool unregister_quality_profile(std::uint64_t id) noexcept {
        return runtime_.unregister_quality_profile(id);
    }
    void set_quality_mutator(QualityMutationCallback callback) {
        backend_.set_quality_mutator(std::move(callback));
    }
    void set_texture_mutator(TextureMutationCallback callback) {
        backend_.set_texture_mutator(std::move(callback));
    }

    // Drain all queued observer events into both the slow-path ResourceGraph
    // and the live runtime bridge. frame_tick also polls completion fences and
    // optionally snapshots the DXGI process budget before running the governor.
    std::size_t drain();
    [[nodiscard]] UnifiedRuntimeTickResult frame_tick(
        FrameBudgetSample frame,
        bool query_budget = true);

    void set_mode(RuntimeMode mode) noexcept { runtime_.set_mode(mode); }
    void reset_circuit_breaker() noexcept { runtime_.reset_circuit_breaker(); }

    [[nodiscard]] ResourceId resource_id(ID3D12Resource* resource) const noexcept;
    [[nodiscard]] HeapId heap_id(ID3D12Heap* heap) const noexcept;
    [[nodiscard]] QueueId queue_id(ID3D12CommandQueue* queue) const noexcept;
    [[nodiscard]] CommandId command_id(ID3D12CommandList* command) const noexcept;
    [[nodiscard]] std::uint64_t fence_id(ID3D12Fence* fence) const noexcept;

    [[nodiscard]] const ResourceGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] ResourceGraph& graph() noexcept { return graph_; }
    [[nodiscard]] RuntimeIntegration& runtime() noexcept { return runtime_; }
    [[nodiscard]] const RuntimeIntegration& runtime() const noexcept { return runtime_; }
    [[nodiscard]] LiveRuntimeBackend& backend() noexcept { return backend_; }
    [[nodiscard]] const LiveRuntimeBackend& backend() const noexcept { return backend_; }
    [[nodiscard]] NativeHostAdapterMetrics metrics() const noexcept;
    [[nodiscard]] std::vector<ResourceSemanticEstimate> semantic_snapshot() const;
    [[nodiscard]] CompatibilityDecision compatibility_snapshot() const;

private:
    struct DescriptorHeapRecord {
        std::uint64_t id{};
        std::uint32_t count{};
        std::uint32_t increment{};
        std::unordered_map<std::uint32_t, DescriptorId> descriptors{};
    };

    struct CompletionBinding {
        QueueId queue{};
        std::uint64_t fence_id{};
        Microsoft::WRL::ComPtr<ID3D12Fence> fence{};
    };

    [[nodiscard]] static QueueClass queue_class(D3D12_COMMAND_LIST_TYPE type) noexcept;
    [[nodiscard]] bool emit_locked(EventType type, const void* payload, std::uint32_t bytes) noexcept;
    [[nodiscard]] DescriptorId descriptor_id_locked(ID3D12DescriptorHeap* heap, std::uint32_t index, bool create);
    [[nodiscard]] bool observe_copy_locked(
        EventType type,
        ID3D12CommandList* command,
        ID3D12Resource* source,
        ID3D12Resource* destination,
        std::uint64_t approximate_bytes);
    [[nodiscard]] std::size_t drain_locked();
    [[nodiscard]] bool note_failure() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_{};
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_{};
    NativeHostAdapterConfig config_{};
    EventRing events_;
    IdAllocator ids_{};
    std::atomic<std::uint64_t> global_sequence_{1};
    Observer observer_;
    ResourceGraph graph_{};
    LiveRuntimeBackend backend_;
    RuntimeIntegration runtime_;

    mutable std::mutex mutex_{};
    std::unordered_map<ID3D12Resource*, ResourceId> resources_{};
    std::unordered_map<ID3D12Heap*, HeapId> heaps_{};
    std::unordered_map<ID3D12DescriptorHeap*, DescriptorHeapRecord> descriptor_heaps_{};
    std::unordered_map<ID3D12CommandQueue*, QueueId> queues_{};
    std::unordered_map<ID3D12CommandList*, CommandId> commands_{};
    std::unordered_map<ID3D12Fence*, std::uint64_t> fences_{};
    std::unordered_map<ID3D12CommandQueue*, CompletionBinding> completion_bindings_{};
    std::unordered_map<ID3D12Resource*, bool> controlled_{};
    std::uint64_t submission_sequence_{};
    FrameId frame_{};
    NativeHostAdapterMetrics metrics_{};
    CompatibilityDecision last_compatibility_{};
};

}  // namespace arc::dx12
