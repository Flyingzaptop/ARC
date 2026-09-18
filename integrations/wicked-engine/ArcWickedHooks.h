#pragma once

#if defined(_WIN32)
#include <d3d12.h>
#include <cstddef>
#include <cstdint>

extern "C" {

void ARCWickedDeviceReady(
    ID3D12Device* device,
    ID3D12DescriptorHeap* resource_heap,
    ID3D12DescriptorHeap* sampler_heap) noexcept;

void ARCWickedResourceCreated(ID3D12Resource* resource) noexcept;
void ARCWickedResourceDestroyed(ID3D12Resource* resource) noexcept;

void ARCWickedObserveSRV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* view) noexcept;

void ARCWickedObserveUAV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* view) noexcept;

void ARCWickedObserveSampler(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index) noexcept;

void ARCWickedCommandBegin(
    ID3D12CommandList* command,
    D3D12_COMMAND_LIST_TYPE type) noexcept;

void ARCWickedResourceUse(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    bool write) noexcept;

void ARCWickedTransition(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before_state,
    D3D12_RESOURCE_STATES after_state,
    std::uint32_t subresource) noexcept;

void ARCWickedCopy(
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    std::uint64_t approximate_bytes,
    std::uint32_t kind) noexcept;

void ARCWickedCountCommand(
    ID3D12CommandList* command,
    std::uint32_t kind,
    std::uint64_t work_items) noexcept;

void ARCWickedSubmit(
    ID3D12CommandQueue* queue,
    ID3D12CommandList* const* commands,
    std::size_t count,
    D3D12_COMMAND_LIST_TYPE type) noexcept;

void ARCWickedPresent(
    std::uint64_t swapchain_id,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    HRESULT result) noexcept;

// Main-thread diagnostic samples only; disabled unless explicitly requested.
void ARCWickedCpuSample(const char* name, double milliseconds) noexcept;
void ARCWickedCommandDestroyed(ID3D12CommandList* command) noexcept;
void ARCWickedQueueDestroyed(ID3D12CommandQueue* queue) noexcept;
void ARCWickedDeviceDestroyed() noexcept;
void ARCWickedResourceTruth(ID3D12Resource* resource, const char* name) noexcept;

}
#endif
