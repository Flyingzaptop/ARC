#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <optional>
#include <cstdint>
namespace arc::dx12::optimizer {
class SpatialProbeGpu {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Resource> reference_,candidate_,result_,readback_;
    Ptr<ID3D12RootSignature> root_;Ptr<ID3D12PipelineState> reset_,compare_;
    Ptr<ID3D12CommandAllocator> allocator_;Ptr<ID3D12GraphicsCommandList> copy_;Ptr<ID3D12Fence> fence_;
    UINT64 capacity_{},sequence_{},expected_epoch_{};bool pending_{},submitted_{},fault_{};
public:
    struct Bin {std::uint32_t error_bits{},valid_tiles{},invalid_tiles{},reserved{};};
    struct Observation {std::uint64_t epoch{};std::uint32_t frame{},outputs{};std::array<Bin,256> bins{};std::array<std::uint32_t,4> errors{};};
    static_assert(sizeof(Observation)==4128);
    SpatialProbeGpu(ID3D12Device*,UINT64 bytes_per_buffer);
    D3D12_GPU_VIRTUAL_ADDRESS reference_address()const{return reference_->GetGPUVirtualAddress();}
    D3D12_GPU_VIRTUAL_ADDRESS candidate_address()const{return candidate_->GetGPUVirtualAddress();}
    UINT64 capacity()const{return capacity_;}
    bool available()const{return !pending_&&!fault_;}
    bool reserve(UINT64 epoch);
    void cancel_unsubmitted(){if(!submitted_)pending_=false;}
    void record_compare(ID3D12GraphicsCommandList*,D3D12_GPU_VIRTUAL_ADDRESS control,D3D12_GPU_VIRTUAL_ADDRESS map,unsigned outputs,bool require_features);
    void submitted(ID3D12CommandQueue*);
    std::optional<Observation> collect();
};
}
