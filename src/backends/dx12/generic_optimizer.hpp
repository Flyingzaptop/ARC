#pragma once
#include <d3d12.h>
#include <mutex>
#include <iosfwd>
#include <array>
#include <vector>
#include "arc/optimizer_policy.hpp"
#include "arc/exact_state_cache.hpp"

namespace arc::dx12::optimizer {
bool enabled() noexcept;
std::uint64_t cpu_nanoseconds() noexcept;
void cpu_snapshot(std::ostream&);
void intercept_cpu_snapshot(std::ostream&);
void control_timing_snapshot(std::ostream&);
void coverage_snapshot(std::ostream&);
struct PolicyStamp {std::uint64_t epoch{},active_submissions{},last_active_epoch{},selected_pipeline{};};
PolicyStamp policy_stamp() noexcept;
struct CandidateCapabilities {std::uint64_t pipeline{};bool coarse{},comparison{},zero{},edges{},mips{};};
CandidateCapabilities candidate_capabilities() noexcept;
struct WorkCandidate {CandidateCapabilities capabilities;double gpu_ms_per_window{};std::uint64_t profile_session{};};
std::vector<WorkCandidate> candidate_catalog() noexcept;
// Validates the entire bundle before publishing any setting. An empty bundle
// is neutral; original-policy configuration remains available through "off".
bool configure_bundle(const arc::PolicyBundle&,bool apply=true) noexcept;
bool cpu_configure(bool) noexcept;
bool cpu_enabled() noexcept;
bool cpu_state(ID3D12GraphicsCommandList*,unsigned slot,const void*,std::size_t) noexcept;
void cpu_invalidate(ID3D12GraphicsCommandList*) noexcept;
void cpu_objects_changed() noexcept;
void cpu_cache_snapshot(std::ostream&);
bool restoration_ready() noexcept;
struct FrameStateSample {std::uint64_t pipeline{},submission{};std::vector<std::array<unsigned,3>> keys;std::vector<std::array<UINT,4>> words;std::vector<unsigned> valid;};
void sample_frame_state(bool enabled) noexcept;
FrameStateSample frame_state_sample();
// Explicit environment-based diagnostic setup, called before application PSOs.
// No environment variables -> disabled, unchanged observer behavior.
bool initialize() noexcept;
bool configure(const wchar_t*) noexcept;
void snapshot(std::ostream&);
void collect() noexcept;
struct DescriptorWrite {std::unique_lock<std::recursive_mutex> lock;};
DescriptorWrite descriptor_write() noexcept;
void root_created(ID3D12RootSignature*,const void*,SIZE_T) noexcept;
void compute_created(ID3D12PipelineState*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*) noexcept;
void stream_created(ID3D12PipelineState*,const D3D12_PIPELINE_STATE_STREAM_DESC*) noexcept;
void signature_created(ID3D12CommandSignature*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*) noexcept;
void after_indirect(ID3D12GraphicsCommandList*,ID3D12CommandSignature*) noexcept;
void heap_created(ID3D12DescriptorHeap*) noexcept;
void resource_created(ID3D12Resource*,ID3D12Heap* heap=nullptr,UINT64 offset=0) noexcept;
void srv(ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void uav(ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void sampler(const D3D12_SAMPLER_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void copy_descriptors(UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,UINT) noexcept;
void descriptor_ranges(UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT) noexcept;
void begin(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void close(ID3D12GraphicsCommandList*) noexcept;
void pipeline(ID3D12GraphicsCommandList*,ID3D12PipelineState*) noexcept;
void heaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap*const*) noexcept;
void root(ID3D12GraphicsCommandList*,ID3D12RootSignature*) noexcept;
void table(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
void constants(ID3D12GraphicsCommandList*,UINT,UINT,const UINT*,UINT) noexcept;
void descriptor(ID3D12GraphicsCommandList*,UINT,D3D12_ROOT_PARAMETER_TYPE,UINT64) noexcept;
void invalidate(ID3D12GraphicsCommandList*) noexcept;
void state_unknown(ID3D12GraphicsCommandList*) noexcept;
void render_pass(ID3D12GraphicsCommandList*,bool,D3D12_RENDER_PASS_FLAGS flags={}) noexcept;
void invalidate_all() noexcept;
bool dispatch(ID3D12GraphicsCommandList*,UINT,UINT,UINT) noexcept;
bool execute(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*) noexcept;
}
