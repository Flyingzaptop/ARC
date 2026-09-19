#pragma once
#include <d3d12.h>
#include <memory>
#include <iosfwd>
#include <cstdint>

namespace arc::dx12::mirror {
struct Lease {std::shared_ptr<void> owner;ID3D12GraphicsCommandList* list{};};
struct InternalCall {bool old;InternalCall() noexcept;~InternalCall();};
bool internal() noexcept;
bool configure(UINT rate) noexcept; // 0: neutral maps; 5: experimental Tier 2 VRS
void begin(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void close(ID3D12GraphicsCommandList*) noexcept;
void invalidate(ID3D12GraphicsCommandList*) noexcept;
void pipeline_created(ID3D12PipelineState*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*) noexcept;
void pipeline_stream_created(ID3D12PipelineState*,const D3D12_PIPELINE_STATE_STREAM_DESC*) noexcept;
void signature_created(ID3D12CommandSignature*,const D3D12_COMMAND_SIGNATURE_DESC*) noexcept;
bool raster_indirect(ID3D12CommandSignature*) noexcept;
void pipeline(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void shading_rate(ID3D12GraphicsCommandList*,D3D12_SHADING_RATE,const D3D12_SHADING_RATE_COMBINER*) noexcept;
void shading_image(ID3D12GraphicsCommandList*,ID3D12Resource*) noexcept;
Lease acquire(ID3D12GraphicsCommandList*) noexcept;
Lease acquire_draw(ID3D12GraphicsCommandList*) noexcept;
bool before_draw(const Lease&) noexcept;
void after_draw(const Lease&) noexcept;
bool execute(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
void collect() noexcept;
void snapshot(std::ostream&);
UINT requested_rate() noexcept;
std::uint64_t modified_draws() noexcept;
template<class Fn,class Self,class... Args> void record(Fn original,Self* self,Args... args){
    // Commands are recorded once on the application's list. Policy changes
    // update a GPU shading-rate image at submission boundaries.
}
}
