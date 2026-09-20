#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace arc::dx12::optimizer {
struct ControlValue {UINT x{1},y{1},width{},height{},comparison_taps{},zero_factor{},edge_sources{};float edge_threshold{};};
// A recording ends by restoring this buffer to neutral on the GPU. Therefore a
// missed update, exhausted upload ring or subsequent cached replay cannot keep
// a coarse policy accidentally. Callers serialize methods with submissions.
class GpuControl {
public:
    static constexpr unsigned capacity=32;
    explicit GpuControl(ID3D12Device*); // worker only: allocation/initialization
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS address(unsigned slot) const noexcept;
    void record_neutralize(ID3D12GraphicsCommandList*);
    ID3D12CommandList* prepare(ID3D12CommandQueue*,std::span<const ControlValue>);
    void submitted(ID3D12CommandQueue*);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] ID3D12Device* device() const noexcept {return device_.Get();}
    void release_completed_queue() noexcept;
    std::vector<std::shared_ptr<void>> keep_alive;
private:
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12Resource> buffer_,upload_,neutral_;
    Ptr<ID3D12Fence> fence_;
    Ptr<ID3D12CommandQueue> last_queue_;
    std::array<Ptr<ID3D12CommandAllocator>,2> allocators_;
    std::array<std::array<Ptr<ID3D12GraphicsCommandList>,4>,2> copies_;
    std::array<UINT64,4> retired_{};
    UINT64 sequence_{};
    int pending_{-1};
    bool fault_{};
};
}
