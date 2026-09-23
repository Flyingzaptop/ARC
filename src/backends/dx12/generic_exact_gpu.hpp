#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>

namespace arc::dx12::exact_gpu {
// Only observed, committed, nonshared DEFAULT buffers are eligible. The
// caller reports creation before any list records use of the resource.
void resource_created(ID3D12Resource*, bool committed_default_nonshared) noexcept;
void resource_cpu_access(ID3D12Resource*) noexcept; // before Map/WriteToSubresource
void correctness_loss() noexcept;
void begin(ID3D12GraphicsCommandList*) noexcept; // successful Create/Reset
void copy(ID3D12GraphicsCommandList*,ID3D12Resource* dst,ID3D12Resource* src) noexcept;
void barriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*) noexcept;
void unknown(ID3D12GraphicsCommandList*) noexcept; // before every unsupported command
void close(ID3D12GraphicsCommandList*,bool succeeded) noexcept;

// All queue ExecuteCommandLists calls, including unrecognized lists, must use
// this boundary. An empty prepared batch represents a proved GPU skip.
struct Submission {
    std::array<ID3D12CommandList*,64> lists{};
    UINT count{};
    std::uint64_t token{};
    bool skipped{},graph_replayed{};
    std::shared_ptr<void> hold{};
};
Submission prepare(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
void submitted(ID3D12CommandQueue*,const Submission&,bool native_call_made) noexcept;
void snapshot(std::ostream&);
}
