#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <iosfwd>
#include <array>
#include <vector>
#include <string>
#include "arc/optimizer_policy.hpp"
#include "arc/exact_state_cache.hpp"
#ifndef CINTERFACE
#include "generic_gpu_control.hpp"
#endif

namespace arc::dx12::optimizer {
bool enabled() noexcept;
std::uint64_t cpu_nanoseconds() noexcept;
void cpu_snapshot(std::ostream&);
void intercept_cpu_snapshot(std::ostream&);
void control_timing_snapshot(std::ostream&);
void coverage_snapshot(std::ostream&);
#ifndef CINTERFACE
std::vector<GpuControl::ExecutionReadback> capture_execution(ID3D12CommandQueue*) noexcept;
#endif
void require_presentation_queue(ID3D12CommandQueue*) noexcept;
void present_frame(std::uint64_t frame) noexcept;
std::uint64_t binding_evidence_revision() noexcept;
bool seal_binding_evidence(std::uint64_t policy) noexcept;
void reset_binding_evidence() noexcept;
void center_priority(bool enabled) noexcept;
void presentation_surface(IDXGISwapChain*) noexcept;
std::vector<std::string> drain_events() noexcept;
void request_spatial_diagnostics() noexcept;
std::string spatial_snapshot() noexcept;
std::array<std::uint64_t,3> control_allocation_bytes() noexcept;
struct ConnectionCoverage {std::size_t roots{},pipelines{};std::uint64_t unknown_root_dispatches{};};
ConnectionCoverage connection_coverage() noexcept;
struct PolicyStamp {std::uint64_t epoch{},active_submissions{},last_active_epoch{},selected_pipeline{};};
PolicyStamp policy_stamp() noexcept;
void spatial_learning(bool enabled,float error_limit) noexcept;
void pause_spatial_probes(bool) noexcept;
bool begin_spatial_training(const arc::PolicyBundle&,std::uint64_t pipeline) noexcept;
void end_spatial_training() noexcept;
struct SpatialProgress {unsigned models{},sampled{},eligible{},probes{};float best_error{1000};};
SpatialProgress spatial_progress() noexcept;
struct CandidateCapabilities {std::uint64_t pipeline{};bool coarse{},comparison{},zero{},edges{},mips{},samples{};};
CandidateCapabilities candidate_capabilities() noexcept;
struct WorkCandidate {CandidateCapabilities capabilities;double gpu_ms_per_window{};std::uint64_t profile_session{};};
std::vector<WorkCandidate> candidate_catalog() noexcept;
// Validates the entire bundle before publishing any setting. An empty bundle
// is neutral; original-policy configuration remains available through "off".
bool configure_bundle(const arc::PolicyBundle&,bool apply=true,std::uint64_t calibration_epoch=0,std::uint64_t valid_until_frame=0) noexcept;
bool begin_calibration(const arc::PolicyBundle&,std::uint64_t epoch) noexcept;
bool configure_bundle_file(const wchar_t*) noexcept;
void calibration_snapshot(std::ostream&);
struct CalibrationCost {std::uint64_t pipeline{},epoch{};double wrapped_ms{},original_ms{},neutralize_ms{},upload_ms{};};
std::vector<CalibrationCost> calibration_costs(std::uint64_t epoch) noexcept;
std::uint64_t calibration_sample_count(std::uint64_t epoch) noexcept;
void predication(ID3D12GraphicsCommandList*,bool enabled) noexcept;
void query_scope(ID3D12GraphicsCommandList*,bool begin) noexcept;
bool cpu_configure(bool) noexcept;
bool cpu_enabled() noexcept;
bool cpu_state(ID3D12GraphicsCommandList*,unsigned slot,const void*,std::size_t) noexcept;
void cpu_invalidate(ID3D12GraphicsCommandList*) noexcept;
void cpu_objects_changed() noexcept;
void cpu_cache_snapshot(std::ostream&);
struct CpuCacheCounters {std::uint64_t skipped{},controlled_submissions{};};
CpuCacheCounters cpu_cache_counters() noexcept;
bool cpu_same_view(unsigned kind,ID3D12Resource*,ID3D12Resource*,const void*,std::size_t,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void forget_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
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
