#include "arc/dx12_runtime_backend.hpp"

namespace arc::dx12 {

LiveRuntimeBackend::LiveRuntimeBackend(ID3D12Device* device)
    : device_(device), residency_(device) {}

bool LiveRuntimeBackend::bind_resource(ResourceId resource, ID3D12Pageable* pageable) {
    if (!resource || !pageable || resources_.contains(resource)) return false;
    Microsoft::WRL::ComPtr<ID3D12Pageable> reference;
    reference = pageable;
    return resources_.emplace(resource, std::move(reference)).second;
}

bool LiveRuntimeBackend::unbind_resource(ResourceId resource) noexcept {
    return resources_.erase(resource) != 0;
}

bool LiveRuntimeBackend::bind_queue_fence(QueueId queue, ID3D12Fence* fence) {
    if (!queue || !fence || queue_fences_.contains(queue)) return false;
    Microsoft::WRL::ComPtr<ID3D12Fence> reference;
    reference = fence;
    return queue_fences_.emplace(queue, std::move(reference)).second;
}

bool LiveRuntimeBackend::unbind_queue_fence(QueueId queue) noexcept {
    return queue_fences_.erase(queue) != 0;
}

RuntimeBackendStatus LiveRuntimeBackend::translate(ResidencyResult result) noexcept {
    switch (result) {
    case ResidencyResult::Success: return RuntimeBackendStatus::Success;
    case ResidencyResult::UnsafeInFlight: return RuntimeBackendStatus::UnsafeInFlight;
    case ResidencyResult::OutOfBudget: return RuntimeBackendStatus::OutOfBudget;
    case ResidencyResult::Timeout: return RuntimeBackendStatus::Timeout;
    case ResidencyResult::InvalidArgument:
    case ResidencyResult::ApiFailure:
        return RuntimeBackendStatus::Failure;
    }
    return RuntimeBackendStatus::Failure;
}

RuntimeBackendStatus LiveRuntimeBackend::evict(ResourceId resource, const ResidencyAction& action) noexcept {
    const auto object = resources_.find(resource);
    if (!device_ || object == resources_.end()) return RuntimeBackendStatus::Unsupported;

    if (action.required_fence == 0) {
        ID3D12Pageable* pageables[] = {object->second.Get()};
        return SUCCEEDED(device_->Evict(1, pageables))
            ? RuntimeBackendStatus::Success
            : RuntimeBackendStatus::Failure;
    }

    const auto fence = queue_fences_.find(action.required_queue);
    if (fence == queue_fences_.end()) return RuntimeBackendStatus::Unsupported;
    return translate(residency_.evict_after(object->second.Get(), fence->second.Get(), action.required_fence));
}

RuntimeBackendStatus LiveRuntimeBackend::make_resident(ResourceId resource, const ResidencyAction&) noexcept {
    const auto object = resources_.find(resource);
    if (object == resources_.end()) return RuntimeBackendStatus::Unsupported;
    return translate(residency_.make_resident_and_wait(object->second.Get()));
}

RuntimeBackendStatus LiveRuntimeBackend::demote_texture(ResourceId resource, const TextureQualityAction& action) noexcept {
    if (action.resource != resource || !texture_mutator_) return RuntimeBackendStatus::Unsupported;
    try {
        return texture_mutator_(action);
    } catch (...) {
        return RuntimeBackendStatus::Failure;
    }
}

RuntimeBackendStatus LiveRuntimeBackend::promote_texture(ResourceId resource, const TextureQualityAction& action) noexcept {
    if (action.resource != resource || !texture_mutator_) return RuntimeBackendStatus::Unsupported;
    try {
        return texture_mutator_(action);
    } catch (...) {
        return RuntimeBackendStatus::Failure;
    }
}

RuntimeBackendStatus LiveRuntimeBackend::apply_quality(const QualityActionCandidate& action) noexcept {
    if (!quality_mutator_) return RuntimeBackendStatus::Unsupported;
    try {
        return quality_mutator_(action, false);
    } catch (...) {
        return RuntimeBackendStatus::Failure;
    }
}

RuntimeBackendStatus LiveRuntimeBackend::restore_quality(const QualityActionCandidate& action) noexcept {
    if (!quality_mutator_) return RuntimeBackendStatus::Unsupported;
    try {
        return quality_mutator_(action, true);
    } catch (...) {
        return RuntimeBackendStatus::Failure;
    }
}

}  // namespace arc::dx12
