#pragma once
#include "arc/resource_graph.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>
namespace arc::dx12 {
enum class ResidencyResult { Success, InvalidArgument, UnsafeInFlight, OutOfBudget, Timeout, ApiFailure };
struct ResidencyTicket { ResidencyResult result{ResidencyResult::ApiFailure}; ID3D12Fence* fence{}; std::uint64_t value{}; };
class ResidencyBackend {
public:
    explicit ResidencyBackend(ID3D12Device* device);
    ResidencyResult evict_after(ID3D12Pageable* object, ID3D12Fence* gpu_fence, std::uint64_t safe_value) noexcept;
    ResidencyResult make_resident_and_wait(ID3D12Pageable* object, std::uint32_t timeout_ms = 10000) noexcept;
    ResidencyTicket enqueue_make_resident(ID3D12Pageable* object) noexcept;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device3> device3_;
    Microsoft::WRL::ComPtr<ID3D12Fence> residency_fence_;
    std::uint64_t next_fence_{1};
};
class BudgetNotification {
public:
    explicit BudgetNotification(IDXGIAdapter3* adapter);
    ~BudgetNotification();
    BudgetNotification(const BudgetNotification&) = delete;
    BudgetNotification& operator=(const BudgetNotification&) = delete;
    [[nodiscard]] bool valid() const noexcept { return event_ != nullptr && cookie_ != 0; }
    [[nodiscard]] std::optional<MemoryBudgetPayload> wait(std::uint32_t timeout_ms) noexcept;
private:
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    HANDLE event_{};
    DWORD cookie_{};
};
}
