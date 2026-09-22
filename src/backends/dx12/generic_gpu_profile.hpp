#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <iosfwd>
#include <vector>

namespace arc::dx12::gpu_profile {
void graphics_created(ID3D12PipelineState*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*) noexcept;
void compute_created(ID3D12PipelineState*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*) noexcept;
void signature_created(ID3D12CommandSignature*,const D3D12_COMMAND_SIGNATURE_DESC*) noexcept;
void indirect(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT max_commands) noexcept;
void stream_created(ID3D12PipelineState*,const D3D12_PIPELINE_STATE_STREAM_DESC*) noexcept;
void begin(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void pipeline(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void work(ID3D12GraphicsCommandList*,bool compute,UINT x,UINT y,UINT z) noexcept;
void opaque(ID3D12GraphicsCommandList*) noexcept;
void render_pass(ID3D12GraphicsCommandList*,bool begin,D3D12_RENDER_PASS_FLAGS flags={}) noexcept;
void protected_session(ID3D12GraphicsCommandList*) noexcept;
void close(ID3D12GraphicsCommandList*) noexcept;

// Holds the readback lock across native submission, so cached replay cannot
// overwrite a mapped sample. It never waits on GPU completion.
struct Submission {
    std::unique_lock<std::recursive_mutex> lock;
    std::array<unsigned short,512> jobs;
    unsigned count{};
};
Submission before_submit(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
void after_submit(Submission&,ID3D12CommandQueue*) noexcept;
bool request(const std::wstring& new_path,UINT present_windows=8) noexcept;
bool busy() noexcept;
bool needs_raster_observation() noexcept;
void stop() noexcept;
void present(void* swapchain=nullptr) noexcept;
struct LoadEvidence {std::uint64_t sequence{},completed_tick_ms{};double frame_ms{},cpu_running_ms{},gpu_queue_busy_ms{};bool cpu_valid{},gpu_valid{};};
LoadEvidence load_evidence() noexcept;
void collect() noexcept;
void snapshot(std::ostream&);
struct ComputeCost {ID3D12PipelineState* pipeline{};std::uint64_t session{},pipeline_identity{};double total_gpu_ms{};UINT present_windows{};};
// Only completed, healthy captures; retired PSOs are never matched by an old
// address. Consumers must call before taking their own submission lock.
std::vector<ComputeCost> compute_costs() noexcept;
std::uint64_t pipeline_identity(ID3D12PipelineState*) noexcept;
}
