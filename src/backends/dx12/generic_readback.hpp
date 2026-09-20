#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <chrono>

namespace arc::dx12::generic {
// One explicitly requested image. No GPU wait or Map occurs on Present.
// Call under the runtime mutex; native calls must suppress our own hooks.
class ImageReadback {
public:
    static bool prepare_feature_device(ID3D12Device*);
    static bool prepare_features(); // compile on request thread, never first-use on Present
    bool enqueue(IDXGISwapChain*, ID3D12CommandQueue*, const std::filesystem::path&,std::uint64_t mutations=0,UINT experimental_rate=0,bool features=false);
    void presented(HRESULT result) noexcept {if(!present_seen_){present_result_=result;present_seen_=true;}}
    bool ready() const noexcept;
    bool write(); // worker only, after ready(); false reports GPU/IO failure
    void color_space(UINT value,bool known) noexcept {color_space_=value;color_space_known_=known;}
    void release_queue_for_cache() noexcept {list_.Reset();allocator_.Reset();queue_.Reset();}
    [[nodiscard]] bool submitted() const noexcept {return submitted_;}
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList> list_;
    Ptr<ID3D12Resource> readback_,timestamps_;
    Ptr<ID3D12QueryHeap> queries_;
    Ptr<ID3D12Fence> fence_;
    Ptr<ID3D12RootSignature> feature_root_;
    Ptr<ID3D12PipelineState> feature_pipeline_;
    Ptr<ID3D12DescriptorHeap> feature_heap_;
    Ptr<ID3D12Resource> feature_output_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    std::filesystem::path path_;
    UINT width_{},height_{},format_{},buffer_index_{};
    UINT64 frequency_{};
    HRESULT signal_result_{E_PENDING},present_result_{E_PENDING};
    bool submitted_{},present_seen_{};
    double enqueue_cpu_ms_{};
    std::uint64_t mutations_{};UINT experimental_rate_{};
    bool features_{};UINT tiles_x_{},tiles_y_{};UINT64 output_bytes_{};
    bool reused_storage_{};
    UINT color_space_{};bool color_space_known_{};UINT64 capture_qpc_{};
};
}
