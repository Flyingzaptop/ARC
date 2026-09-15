#include "arc/dx12_residency.hpp"
#include "arc/dx12_observer.hpp"
namespace arc::dx12 {
ResidencyBackend::ResidencyBackend(ID3D12Device* device) : device_(device) {
    if (!device_) { return; }
    if (FAILED(device_.As(&device3_))) { return; }
    (void)device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&residency_fence_));
}
ResidencyResult ResidencyBackend::evict_after(ID3D12Pageable* object, ID3D12Fence* gpuFence, std::uint64_t safeValue) noexcept {
    if (!device_ || !object || !gpuFence) { return ResidencyResult::InvalidArgument; }
    if (gpuFence->GetCompletedValue() < safeValue) { return ResidencyResult::UnsafeInFlight; }
    ID3D12Pageable* objects[] = {object};
    const auto result = device_->Evict(1, objects);
    return SUCCEEDED(result) ? ResidencyResult::Success : ResidencyResult::ApiFailure;
}
ResidencyResult ResidencyBackend::make_resident_and_wait(ID3D12Pageable* object, std::uint32_t timeoutMs) noexcept {
    const auto ticket = enqueue_make_resident(object);
    if (ticket.result != ResidencyResult::Success) { return ticket.result; }
    if (ticket.fence->GetCompletedValue() >= ticket.value) { return ResidencyResult::Success; }
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) { return ResidencyResult::ApiFailure; }
    const auto armed = ticket.fence->SetEventOnCompletion(ticket.value, ready);
    const auto wait = SUCCEEDED(armed) ? WaitForSingleObject(ready, timeoutMs) : WAIT_FAILED;
    CloseHandle(ready);
    return wait == WAIT_OBJECT_0 ? ResidencyResult::Success : wait == WAIT_TIMEOUT ? ResidencyResult::Timeout : ResidencyResult::ApiFailure;
}
ResidencyTicket ResidencyBackend::enqueue_make_resident(ID3D12Pageable* object) noexcept {
    if (!device3_ || !residency_fence_ || !object) { return {ResidencyResult::InvalidArgument}; }
    ID3D12Pageable* objects[] = {object}; const auto value = next_fence_++;
    const auto result = device3_->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET, 1, objects, residency_fence_.Get(), value);
    if (result == E_OUTOFMEMORY) { return {ResidencyResult::OutOfBudget}; }
    if (FAILED(result)) { return {ResidencyResult::ApiFailure}; }
    return {ResidencyResult::Success, residency_fence_.Get(), value};
}
BudgetNotification::BudgetNotification(IDXGIAdapter3* adapter) : adapter_(adapter) {
    if (!adapter_) { return; }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_ || FAILED(adapter_->RegisterVideoMemoryBudgetChangeNotificationEvent(event_, &cookie_))) {
        if (event_) { CloseHandle(event_); event_ = nullptr; } cookie_ = 0;
    }
}
BudgetNotification::~BudgetNotification() {
    if (adapter_ && cookie_) { adapter_->UnregisterVideoMemoryBudgetChangeNotification(cookie_); }
    if (event_) { CloseHandle(event_); }
}
std::optional<MemoryBudgetPayload> BudgetNotification::wait(std::uint32_t timeoutMs) noexcept {
    if (!valid()) { return std::nullopt; }
    if (WaitForSingleObject(event_, timeoutMs) != WAIT_OBJECT_0) { return std::nullopt; }
    return query_memory_budget(adapter_.Get());
}
}
