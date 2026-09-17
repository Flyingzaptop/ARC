#pragma once

#include "arc/dx12_residency.hpp"
#include "arc/runtime_coordinator.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <functional>
#include <unordered_map>
#include <utility>

namespace arc::dx12 {

using TextureMutationCallback = std::function<RuntimeBackendStatus(const TextureQualityAction&)>;
using QualityMutationCallback = std::function<RuntimeBackendStatus(const QualityActionCandidate&, bool restore)>;

// D3D12 implementation of the backend-neutral runtime mutation contract.
// Whole-resource residency is handled directly. Texture quality transitions
// remain host-provided because the owner of a reserved/tiled resource knows
// its tile heaps and mapping topology. Generic quality actions use the same
// explicit host binding model: ARC decides, the renderer owns the physical
// mapping from a quality action to its D3D12 work/resources.
class LiveRuntimeBackend final : public arc::RuntimeMutationBackend {
public:
    explicit LiveRuntimeBackend(ID3D12Device* device);

    bool bind_resource(ResourceId resource, ID3D12Pageable* pageable);
    bool unbind_resource(ResourceId resource) noexcept;
    bool bind_queue_fence(QueueId queue, ID3D12Fence* fence);
    bool unbind_queue_fence(QueueId queue) noexcept;
    void set_texture_mutator(TextureMutationCallback callback) { texture_mutator_ = std::move(callback); }
    void set_quality_mutator(QualityMutationCallback callback) { quality_mutator_ = std::move(callback); }

    [[nodiscard]] bool has_resource(ResourceId resource) const noexcept { return resources_.contains(resource); }
    [[nodiscard]] bool has_queue_fence(QueueId queue) const noexcept { return queue_fences_.contains(queue); }
    [[nodiscard]] bool has_quality_mutator() const noexcept { return static_cast<bool>(quality_mutator_); }

    RuntimeBackendStatus evict(ResourceId resource, const ResidencyAction& action) noexcept override;
    RuntimeBackendStatus make_resident(ResourceId resource, const ResidencyAction& action) noexcept override;
    RuntimeBackendStatus demote_texture(ResourceId resource, const TextureQualityAction& action) noexcept override;
    RuntimeBackendStatus promote_texture(ResourceId resource, const TextureQualityAction& action) noexcept override;
    RuntimeBackendStatus apply_quality(const QualityActionCandidate& action) noexcept override;
    RuntimeBackendStatus restore_quality(const QualityActionCandidate& action) noexcept override;

private:
    static RuntimeBackendStatus translate(ResidencyResult result) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_{};
    ResidencyBackend residency_;
    std::unordered_map<ResourceId, Microsoft::WRL::ComPtr<ID3D12Pageable>> resources_{};
    std::unordered_map<QueueId, Microsoft::WRL::ComPtr<ID3D12Fence>> queue_fences_{};
    TextureMutationCallback texture_mutator_{};
    QualityMutationCallback quality_mutator_{};
};

}  // namespace arc::dx12
