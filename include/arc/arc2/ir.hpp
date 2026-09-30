#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arc::arc2 {

struct ObjectId { std::uint64_t value{}; friend bool operator==(ObjectId, ObjectId) = default; friend auto operator<=>(ObjectId, ObjectId) = default; explicit operator bool() const { return value != 0; } };
struct WorkId { std::uint64_t value{}; friend bool operator==(WorkId, WorkId) = default; explicit operator bool() const { return value != 0; } };
struct SubmissionId { std::uint64_t value{}; friend bool operator==(SubmissionId, SubmissionId) = default; explicit operator bool() const { return value != 0; } };
enum class ObjectKind : std::uint8_t { Device, Queue, Allocator, CommandList, Fence, Heap, Resource, DescriptorHeap, RootSignature, PipelineState, Shader, Swapchain, CommandSignature, QueryHeap };
enum class ViewKind : std::uint8_t { Unknown, Cbv, Srv, Uav, Rtv, Dsv, Sampler };
enum class WorkKind : std::uint8_t { Draw, DrawIndexed, Dispatch, ExecuteIndirect, Copy, Resolve, Clear, Barrier, Bundle, Query, Unknown };
enum class AccessKind : std::uint8_t { Read, Write, ReadWrite, Unknown };
enum class Certainty : std::uint8_t { Known, Symbolic, Unknown };
enum class BindingKind : std::uint8_t { Cbv, Srv, Uav, Sampler };
enum class RootParameterKind : std::uint8_t { Table, Constants, Descriptor };
struct DescriptorRange { BindingKind kind{BindingKind::Srv}; std::uint32_t space{}, first_register{}, count{}, table_offset{}; };
struct RootParameter { std::uint32_t slot{}; RootParameterKind kind{RootParameterKind::Table}; BindingKind descriptor_kind{BindingKind::Srv}; std::uint32_t space{}, shader_register{}, constant_count{}; std::vector<DescriptorRange> ranges; };
struct ShaderBinding { BindingKind kind{BindingKind::Srv}; std::uint32_t space{}, first_register{}, count{}; bool dynamic_indexing{}; AccessKind access{AccessKind::Read}; };
struct DescriptorRef { ObjectId heap{}; std::uint32_t index{}; std::uint64_t generation{}; friend bool operator==(DescriptorRef, DescriptorRef) = default; };
struct Descriptor { DescriptorRef ref{}; ObjectId resource{}; ViewKind kind{ViewKind::Unknown}; bool valid{}; };
struct Access { ObjectId resource{}; AccessKind kind{AccessKind::Unknown}; Certainty certainty{Certainty::Unknown}; std::uint64_t offset{}, bytes{}; std::string symbol; ObjectId descriptor_heap{}; std::uint64_t descriptor_first{}, descriptor_count{}; };
struct RootBinding { enum class Kind : std::uint8_t { Table, Descriptor, Constants }; Kind kind{Kind::Table}; DescriptorRef table{}; ObjectId resource{}; std::uint64_t address{}; std::vector<std::uint32_t> constants; std::optional<BindingKind> descriptor_kind; };
struct BufferBinding { ObjectId resource{}; std::uint64_t offset{}, bytes{}; };
struct Rect { std::int32_t x{}, y{}, width{}, height{}; };
struct Viewport { float x{}, y{}, width{}, height{}, min_depth{}, max_depth{}; };
struct FixedTargetBlend { bool blend_enable{}, logic_enable{}; std::uint32_t src_color{}, dst_color{}, color_op{}, src_alpha{}, dst_alpha{}, alpha_op{}, logic_op{}, write_mask{}; };
struct FixedStencilFace { std::uint32_t fail_op{}, depth_fail_op{}, pass_op{}, compare_op{}; };
struct FixedPipelineState {
    bool raster_known{}, depth_known{}, blend_known{}, topology_known{};
    std::uint32_t raster_fill{}, raster_cull{};
    bool front_ccw{}, depth_clip{}, conservative_raster{};
    std::int32_t depth_bias{};
    float depth_bias_clamp{}, slope_scaled_depth_bias{};
    bool multisample_enable{}, antialiased_line_enable{};
    std::uint32_t forced_sample_count{};
    bool depth_enable{}, depth_write{}, stencil_enable{};
    std::uint32_t depth_func{}, stencil_read_mask{}, stencil_write_mask{};
    FixedStencilFace front_stencil{}, back_stencil{};
    std::uint32_t topology_type{}, sample_mask{}, sample_count{};
    bool alpha_to_coverage{}, independent_blend{};
    std::array<FixedTargetBlend,8> targets{};
};
struct PipelineSnapshot {
    ObjectId pipeline{}, graphics_root{}, compute_root{};
    std::vector<ObjectId> descriptor_heaps;
    std::map<std::uint32_t, RootBinding> graphics_bindings, compute_bindings;
    std::map<std::uint32_t, BufferBinding> vertex_buffers;
    BufferBinding index_buffer{};
    std::vector<DescriptorRef> render_targets;
    DescriptorRef depth_target{};
    std::optional<Rect> scissor;
    std::vector<Viewport> viewports;
    FixedPipelineState fixed;
    std::uint32_t topology{}, stencil_ref{};
    bool topology_known{}, stencil_ref_known{}, blend_factor_known{};
    std::array<float,4> blend_factor{};
    std::vector<ObjectId> shaders;
    std::uint32_t shading_rate{};
    std::vector<std::uint32_t> shading_rate_combiners;
    bool shading_rate_known{};
    bool raster_known{}, depth_known{}, blend_known{};
};
struct WorkItem { WorkId id{}; ObjectId list{}; std::uint64_t list_generation{}; WorkKind kind{WorkKind::Unknown}; PipelineSnapshot state; std::vector<Access> accesses; std::vector<WorkId> dependencies; bool supported{true}, rewrite_eligible{}; std::string coverage; };
struct Submission { SubmissionId id{}; ObjectId queue{}; std::vector<WorkId> work; std::vector<SubmissionId> dependencies; bool complete{}; };
struct FenceEdge { ObjectId queue{}, fence{}; std::uint64_t value{}; SubmissionId submission{}; bool signal{}; bool satisfied{}; };
struct Transition { WorkId work{}; ObjectId resource{}; std::uint64_t before{}, after{}; bool known{}; bool aliasing{}; };
struct Present { ObjectId swapchain{}, queue{}, backbuffer{}; std::uint64_t sequence{}, swapchain_generation{}; SubmissionId reachable{}; std::int32_t result{}; };
struct ObjectRecord { ObjectId id{}; ObjectKind kind{}; std::uintptr_t native_identity{}; bool alive{}; };
enum class ResourceDimension : std::uint8_t { Unknown, Buffer, Texture1D, Texture2D, Texture3D };
enum class ResourceAllocation : std::uint8_t { Unknown, Committed, Placed, Reserved, External };
struct ResourceShape { ResourceDimension dimension{ResourceDimension::Unknown}; ResourceAllocation allocation{ResourceAllocation::Unknown}; std::uint64_t width{}; std::uint32_t height{}, depth{}, array_layers{}, mips{}, format{}, samples{}, flags{}; std::uint32_t format_namespace{}, flags_namespace{}; };
struct ResourceDescription { ObjectId resource{}, heap{}; std::uint64_t bytes{}, offset{}; ResourceShape shape; };
struct ShaderDescription { ObjectId shader{}; std::vector<Access> declared; std::vector<ShaderBinding> bindings; };
struct RootSignatureDescription { ObjectId root{}; std::vector<RootParameter> parameters; };
struct IrSnapshot { std::vector<ObjectRecord> objects; std::vector<ResourceDescription> resources; std::vector<ShaderDescription> shaders; std::vector<RootSignatureDescription> root_signatures; std::vector<Descriptor> descriptors; std::vector<WorkItem> work; std::vector<Submission> submissions; std::vector<FenceEdge> fences; std::vector<Transition> transitions; std::vector<Present> presents; std::uint64_t total_work{}, dropped{}, uncertain_submissions{}; bool incomplete{}, history_truncated{}; };
std::string serialize(const IrSnapshot& snapshot);

} // namespace arc::arc2
