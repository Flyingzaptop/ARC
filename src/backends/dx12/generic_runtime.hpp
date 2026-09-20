#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <cstdint>
#include <iosfwd>
#include <array>

namespace arc::dx12::generic {
// Called only after successful native operations; no raw pointer is kept alive
// by the observer. ID3D12Object private lifetime tokens own identity retirement.
void observe_resource(ID3D12Resource*) noexcept;
void observe_heap(ID3D12DescriptorHeap*) noexcept;
void observe_view(ID3D12Resource*,D3D12_CPU_DESCRIPTOR_HANDLE,UINT kind,UINT first_mip,UINT mips) noexcept;
void observe_cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void copy_descriptors(UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE,UINT stride) noexcept;
void begin(ID3D12GraphicsCommandList*) noexcept;
void close(ID3D12GraphicsCommandList*) noexcept;
void pipeline(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void targets(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*) noexcept;
void viewport(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*) noexcept;
void scissor(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*) noexcept;
void descriptor_heaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap*const*) noexcept;
void root_table(ID3D12GraphicsCommandList*,bool compute,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
void root_signature(ID3D12GraphicsCommandList*,bool compute,ID3D12RootSignature*) noexcept;
void work(ID3D12GraphicsCommandList*,UINT kind,std::uint64_t items) noexcept;
void copy(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,UINT64 bytes,bool full) noexcept;
void clear_target(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,bool full) noexcept;
void submit(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
void fence(ID3D12CommandQueue*,ID3D12Fence*,UINT64,bool wait) noexcept;
void descriptor_ranges(UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE,UINT stride) noexcept;
void begin_capture() noexcept;
void end_capture(const std::filesystem::path&) noexcept;
void snapshot(std::ostream&);
void unsupported() noexcept;
void observe_swapchain(IDXGISwapChain*,IUnknown*) noexcept;
void observe_color_space(IDXGISwapChain*,DXGI_COLOR_SPACE_TYPE) noexcept;
void invalidate_color_spaces() noexcept;
std::uint64_t before_present(IDXGISwapChain*) noexcept;
void after_present(IDXGISwapChain*,std::uint64_t,HRESULT,UINT flags) noexcept;
bool request_frame(const std::filesystem::path&) noexcept;
bool request_image(const std::filesystem::path&,bool features=false) noexcept;
// Three consecutive real Presents of one known swapchain. Disk writes and
// readback completion never delay the next capture. Ordinary requests are
// excluded until all three jobs finish. Progress means submitted, not written.
bool request_image_sequence(const std::array<std::filesystem::path,3>&,IDXGISwapChain*) noexcept;
unsigned image_sequence_progress() noexcept;
void flush_image() noexcept;
bool gpu_helpers_idle() noexcept;
bool request_timing(const std::filesystem::path&,UINT seconds=60) noexcept;
void flush_timing() noexcept;
void observation_mode_changed() noexcept;
void flush_capture() noexcept;
}
