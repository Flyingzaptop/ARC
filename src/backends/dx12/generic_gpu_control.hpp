#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <span>
#include <vector>
#include <optional>

namespace arc::dx12::optimizer {
struct ControlValue {UINT x{1},y{1},width{},height{},comparison_taps{},zero_factor{},edge_sources{};float edge_threshold{};UINT mip_steps{};alignas(8) UINT64 calibration{},calibration_pipeline{},reserved{},proof_epoch{},proof_pipeline{};UINT spatial_capacity{},spatial_frame{};UINT64 spatial_key{};UINT spatial_tile_width{},spatial_tile_height{},spatial_flags{};float spatial_center{};};
// A recording ends by restoring this buffer to neutral on the GPU. Therefore a
// missed update, exhausted upload ring or subsequent cached replay cannot keep
// a coarse policy accidentally. Callers serialize methods with submissions.
class GpuControl {
public:
    static constexpr unsigned capacity=32;
    static constexpr UINT spatial_capacity=8192;
    static constexpr UINT spatial_record_bytes=32;
    explicit GpuControl(ID3D12Device*,bool measure=false,bool spatial=false); // worker only
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS address(unsigned slot) const noexcept;
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS marker_address(unsigned slot) const noexcept;
    void marker_barrier(ID3D12GraphicsCommandList*);
    struct ExecutionReadback {
        std::shared_ptr<void> lease;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 completion{},offset{},queue{};
        std::array<std::array<UINT64,2>,capacity> expected{};
        bool read(std::array<std::array<UINT64,2>,capacity>& actual,std::array<std::array<UINT,2>,capacity>* spatial=nullptr)const noexcept;
    };
    std::optional<ExecutionReadback> execution_readback(ID3D12CommandQueue*)const;
    struct SpatialTile {UINT64 key{};UINT frame{},mode{};float importance{},feature{},confidence{},reserved{};};
    struct SpatialReadback {
        std::shared_ptr<void> lease;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 completion{},offset{},pipeline{},key{},queue{};
        UINT width{},height{},tile_width{},tile_height{},frame{};
        float threshold{};
        bool read(std::vector<SpatialTile>&)const noexcept;
    };
    std::optional<SpatialReadback> spatial_readback()const{return latest_spatial_;}
    std::array<UINT64,3> allocation_bytes()const noexcept{return allocation_bytes_;} // default/upload/readback
    void begin_recording() noexcept {calibration_mask_=0;}
    bool calibration_available()const noexcept{return bool(calibration_queries_);}
    void calibration_mark(ID3D12GraphicsCommandList*,unsigned slot,unsigned point);
    void calibration_predicate(ID3D12GraphicsCommandList*,unsigned slot);
    void record_neutralize(ID3D12GraphicsCommandList*);
    ID3D12CommandList* prepare(ID3D12CommandQueue*,std::span<const ControlValue>);
    void submitted(ID3D12CommandQueue*);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] ID3D12Device* device() const noexcept {return device_.Get();}
    void release_completed_queue() noexcept;
    struct Timing {UINT64 samples{},dropped{},invalid{};double milliseconds{};};
    void collect_timing() noexcept; // worker only; never waits for GPU completion
    [[nodiscard]] Timing timing() const noexcept {return timing_;}
    struct Calibration {UINT64 epoch{},pipeline{};unsigned slot{};double wrapped_ms{},original_ms{},neutralize_ms{},upload_ms{};};
    const std::vector<Calibration>& calibrations() const noexcept{return calibrations_;}
    std::vector<std::shared_ptr<void>> keep_alive;
private:
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12Resource> buffer_,upload_,neutral_;
    Ptr<ID3D12Resource> marker_,marker_readback_;
    Ptr<ID3D12Resource> spatial_readback_;
    std::array<ControlValue,4> spatial_values_{};
    std::optional<SpatialReadback> latest_spatial_;
    std::array<UINT64,3> allocation_bytes_{};
    std::array<std::array<Ptr<ID3D12GraphicsCommandList>,4>,2> marker_copies_;
    std::array<std::shared_ptr<void>,4> marker_leases_;
    std::array<std::array<std::array<UINT64,2>,capacity>,4> marker_expected_{};
    int last_marker_slot_{-1};
    UINT64 marker_stride_{32};
    Ptr<ID3D12Fence> fence_;
    Ptr<ID3D12QueryHeap> timestamps_;
    Ptr<ID3D12Resource> readback_;
    Ptr<ID3D12QueryHeap> calibration_queries_;
    Ptr<ID3D12Resource> calibration_scratch_,calibration_readback_;
    std::array<std::array<Ptr<ID3D12GraphicsCommandList>,4>,2> calibration_copies_;
    std::array<std::array<UINT64,capacity>,4> calibration_epochs_{};
    std::array<std::array<UINT64,capacity>,4> calibration_pipelines_{};
    std::vector<Calibration> calibrations_;
    UINT calibration_mask_{};
    std::array<UINT64,4> frequencies_{};
    std::array<bool,4> unread_{};
    Timing timing_;
    UINT64 pending_frequency_{};
    Ptr<ID3D12CommandQueue> last_queue_;
    std::array<Ptr<ID3D12CommandAllocator>,2> allocators_;
    std::array<std::array<Ptr<ID3D12GraphicsCommandList>,4>,2> copies_;
    std::array<UINT64,4> retired_{};
    UINT64 sequence_{};
    int pending_{-1};
    bool fault_{};
};
}
