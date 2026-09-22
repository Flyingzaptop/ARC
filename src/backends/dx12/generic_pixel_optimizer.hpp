#pragma once
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <array>
#include <iosfwd>
namespace arc::dx12::pixel {
void root_created(ID3D12RootSignature*,const void*,SIZE_T) noexcept;
void created(ID3D12PipelineState*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*) noexcept;
void begin(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void close(ID3D12GraphicsCommandList*) noexcept;
void invalidate(ID3D12GraphicsCommandList*) noexcept;
void pipeline(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void root(ID3D12GraphicsCommandList*,ID3D12RootSignature*) noexcept;
void table(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
void constants(ID3D12GraphicsCommandList*,UINT,UINT,const UINT*,UINT) noexcept;
void descriptor(ID3D12GraphicsCommandList*,UINT,D3D12_ROOT_PARAMETER_TYPE,UINT64) noexcept;
void heaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap*const*) noexcept;
bool before_draw(ID3D12GraphicsCommandList*) noexcept;
void after_draw(ID3D12GraphicsCommandList*) noexcept;
struct Submission {std::unique_lock<std::recursive_mutex> lock;std::array<std::shared_ptr<void>,8> controls;unsigned count{};};
Submission before_submit(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
void after_submit(Submission&,ID3D12CommandQueue*) noexcept;
unsigned requested_steps() noexcept;
bool configure(unsigned half_steps) noexcept;
bool available() noexcept;
bool ready() noexcept;
bool restoration_ready() noexcept;
void snapshot(std::ostream&);
}
