#pragma once
#include "arc/arc2/ir.hpp"
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arc::arc2 {

class Runtime final {
public:
    explicit Runtime(std::size_t max_work = 65536, std::size_t max_history = 65536);
    ObjectId create_object(ObjectKind kind, std::uintptr_t native_identity);
    void destroy_object(ObjectId id);
    ObjectId find_object(std::uintptr_t native_identity) const;
    void describe_resource(ObjectId resource, std::uint64_t bytes, ObjectId heap = {}, std::uint64_t offset = 0);
    void describe_resource_shape(ObjectId resource, ResourceShape shape);
    void describe_shader(ObjectId shader, std::span<const Access> declared);
    void describe_shader_bindings(ObjectId shader, std::span<const ShaderBinding> bindings);
    void describe_root_signature(ObjectId root, std::span<const RootParameter> parameters);
    void describe_descriptor_heap(ObjectId heap, std::uint32_t count);
    void set_pipeline_shaders(ObjectId pso, std::span<const ObjectId> shaders);
    void describe_pipeline_fixed(ObjectId pso, FixedPipelineState state);
    DescriptorRef write_descriptor(ObjectId heap, std::uint32_t index, ObjectId resource, ViewKind kind);
    DescriptorRef copy_descriptor(ObjectId dst_heap, std::uint32_t dst_index, DescriptorRef source);
    DescriptorRef descriptor(ObjectId heap, std::uint32_t index) const;
    void reset_command_list(ObjectId list, ObjectId allocator, ObjectId pso = {});
    void close_command_list(ObjectId list);
    void set_pipeline(ObjectId list, ObjectId pso);
    void set_root_signature(ObjectId list, ObjectId root, bool compute);
    void set_descriptor_heaps(ObjectId list, std::span<const ObjectId> heaps);
    void set_root_table(ObjectId list, bool compute, std::uint32_t slot, DescriptorRef base);
    void set_root_descriptor(ObjectId list, bool compute, std::uint32_t slot, ObjectId resource, std::uint64_t address, std::optional<BindingKind> kind = {});
    void set_root_constants(ObjectId list, bool compute, std::uint32_t slot, std::uint32_t offset, std::span<const std::uint32_t> values);
    void set_vertex_buffer(ObjectId list, std::uint32_t slot, ObjectId resource, std::uint64_t offset, std::uint64_t bytes, std::optional<std::uint32_t> stride = {});
    void set_index_buffer(ObjectId list, ObjectId resource, std::uint64_t offset, std::uint64_t bytes, std::optional<std::uint32_t> format = {}, std::uint32_t format_namespace = 0);
    void set_targets(ObjectId list, std::span<const DescriptorRef> rt, DescriptorRef depth = {});
    void set_scissor(ObjectId list, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);
    void set_scissors(ObjectId list, std::span<const ScissorRect> rectangles);
    void unknown_scissors(ObjectId list);
    void set_viewports(ObjectId list, std::span<const Viewport> viewports);
    void set_topology(ObjectId list, std::uint32_t normalized_topology);
    void set_blend_factor(ObjectId list, std::array<float,4> factor);
    void set_stencil_ref(ObjectId list, std::uint32_t reference);
    void set_shading_rate(ObjectId list, std::uint32_t rate, std::span<const std::uint32_t> combiners = {});
    WorkId record_work(ObjectId list, WorkKind kind, std::span<const Access> explicit_access = {}, WorkArguments arguments = {});
    bool record_clear(ObjectId list, DescriptorRef target, std::span<const std::uint32_t> exact_signature);
    void barrier(ObjectId list, ObjectId resource, std::uint64_t before, std::uint64_t after, bool known = true);
    void alias_barrier(ObjectId list, ObjectId before, ObjectId after);
    void execute_bundle(ObjectId list, ObjectId bundle);
    void query(ObjectId list, ObjectId query_heap, ObjectId destination = {});
    SubmissionId submit(ObjectId queue, std::span<const ObjectId> lists);
    void signal(ObjectId queue, ObjectId fence, std::uint64_t value);
    void wait(ObjectId queue, ObjectId fence, std::uint64_t value);
    void present(ObjectId swapchain, ObjectId queue = {});
    void present(ObjectId swapchain, ObjectId queue, ObjectId backbuffer, std::int32_t result, bool test_only = false);
    void resize_swapchain(ObjectId swapchain);
    void unsupported(ObjectId list, std::string_view api);
    void note_coverage(std::string_view api);
    void touch_command(ObjectId list);
    std::shared_ptr<const WorkItem> last_work(ObjectId list) const;
    std::uint64_t state_sequence(ObjectId list) const;
    IrSnapshot snapshot() const;
private:
    struct List { ObjectId allocator{}; std::uint64_t generation{1}, sequence{}; bool closed{}, poisoned{}; PipelineSnapshot state; std::vector<WorkId> work; std::vector<std::shared_ptr<const WorkItem>> recorded; DescriptorRef last_clear_target{}; std::vector<std::uint32_t> last_clear_signature; bool last_was_clear{}; };
    struct Resource { std::uint64_t bytes{}, offset{}; ObjectId heap{}; ResourceShape shape; };
    struct Slot { Descriptor current{}; std::uint64_t generation{}; };
    struct Key { ObjectId heap{}; std::uint32_t index{}; friend bool operator==(Key, Key) = default; };
    struct KeyHash { std::size_t operator()(Key k) const noexcept { return std::hash<std::uint64_t>{}(k.heap.value ^ (std::uint64_t(k.index) << 32)); } };
    bool valid(ObjectId id, ObjectKind kind) const;
    void touch(ObjectId id);
    WorkId record_locked(ObjectId list, WorkKind kind, std::span<const Access> access, bool rewrite_eligible = false, std::string_view coverage = {}, WorkArguments arguments = {});
    void push_accesses(WorkItem& item);
    bool descriptors_current(const WorkItem& item) const;
    mutable std::mutex mutex_;
    std::size_t max_work_{}, max_history_{};
    std::uint64_t next_object_{1}, next_work_{1}, next_submission_{1}, next_present_{1}, dropped_{}, uncertain_submissions_{};
    bool incomplete_{}, history_truncated_{};
    std::map<ObjectId, ObjectRecord> objects_;
    std::unordered_map<std::uintptr_t, ObjectId> native_;
    std::map<ObjectId, List> lists_;
    std::unordered_map<Key, Slot, KeyHash> slots_;
    std::map<ObjectId, Resource> resources_;
    std::map<ObjectId, std::vector<Access>> shaders_;
    std::map<ObjectId, std::vector<ShaderBinding>> shader_bindings_;
    std::map<ObjectId, std::vector<RootParameter>> root_parameters_;
    std::map<ObjectId, std::uint32_t> descriptor_heap_counts_;
    std::map<ObjectId, std::vector<ObjectId>> pipeline_shaders_;
    std::map<std::string, std::uint64_t> interface_coverage_;
    std::map<ObjectId, FixedPipelineState> pipeline_fixed_;
    std::deque<std::shared_ptr<const WorkItem>> work_;
    std::deque<Submission> submissions_;
    std::deque<FenceEdge> fences_;
    std::deque<Transition> transitions_;
    std::deque<Present> presents_;
    std::map<ObjectId, SubmissionId> queue_last_;
    std::map<ObjectId, std::map<std::uint64_t, SubmissionId>> fence_signals_;
    std::map<ObjectId, std::vector<SubmissionId>> queue_waits_;
    struct PendingWait { ObjectId queue{}, fence{}; std::uint64_t value{}; SubmissionId first_after{}; };
    std::vector<PendingWait> pending_waits_;
    std::map<ObjectId, std::uint64_t> swapchain_generations_;
};
Runtime& runtime();
} // namespace arc::arc2
