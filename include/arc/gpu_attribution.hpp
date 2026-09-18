#pragma once

#include "arc/ids.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace arc {
using WorkId = std::uint64_t;
enum class GpuWorkKind { Draw, Dispatch, Indirect, Copy, Clear, Present };
// Possible means a binding/candidate, never proof that a shader accessed it.
enum class AccessEvidence { Possible, Observed };
struct WorkAccess {
    ResourceId resource{}; bool write{}; AccessEvidence evidence{AccessEvidence::Possible};
    // Only proven complete overwrite may kill earlier versions. Default retains them.
    bool full_overwrite{};
};
struct ScreenRect { double x{}, y{}, width{}, height{}; };
struct RasterRegion {
    std::uint32_t width{}, height{};
    ScreenRect viewport{}, scissor{};
    bool known{};
};
// Local raster bound, NOT visibility, importance or a final-screen bound after composition.
[[nodiscard]] std::optional<double> raster_coverage_upper(const RasterRegion&) noexcept;
struct GpuTimestampSample {
    std::uint64_t begin{}, end{}, frequency{};
    bool completed{};
};
[[nodiscard]] std::optional<double> gpu_duration_ms(const GpuTimestampSample&) noexcept;
struct WorkObservation {
    GpuWorkKind kind{GpuWorkKind::Draw};
    std::vector<WorkAccess> accesses;
    RasterRegion raster{};
    std::uint64_t items{}, copy_bytes{}, pipeline{};
    bool bindings_complete{};
};
struct AttributionNode {
    WorkId id{}; QueueId queue{}; CommandId command{};
    WorkObservation work;
    std::optional<double> local_coverage_upper, gpu_ms;
    bool unresolved_inputs{};
};
struct AttributionEdge {
    WorkId producer{}, consumer{}; ResourceId resource{};
    bool synchronized{}, observed{};
};
struct AttributionLimits {
    std::size_t commands{256}, works_per_command{4096}, accesses_per_work{64};
    std::size_t nodes{16384}, edges{131072}, resources{8192}, queues{16}, signals{256};
};
struct AttributionSlice {
    std::vector<WorkId> contributors;
    bool complete{};
};
// Explicit, serialized cooperative observer. API order within a command is
// recording order; only submit creates execution nodes. Capture-local IDs.
// Limits fail closed and require clear() before another trustworthy capture.
class GpuAttributionGraph {
public:
    explicit GpuAttributionGraph(AttributionLimits limits = {}) : limits_(limits) {}
    void clear();
    void invalidate() noexcept { ++errors_; }
    bool begin(CommandId);
    bool record(CommandId, const WorkObservation&);
    bool close(CommandId);
    void retire_command(CommandId);
    std::vector<WorkId> submit(QueueId, CommandId);
    bool signal(QueueId, std::uint64_t fence, std::uint64_t value);
    bool wait(QueueId, std::uint64_t fence, std::uint64_t value);
    WorkId present(QueueId, ResourceId);
    bool timing(WorkId, const GpuTimestampSample&);
    [[nodiscard]] AttributionSlice ancestors(WorkId) const;
    [[nodiscard]] bool complete() const noexcept { return errors_ == 0; }
    [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }
    [[nodiscard]] const auto& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const auto& edges() const noexcept { return edges_; }
    [[nodiscard]] std::size_t command_count() const noexcept { return commands_.size(); }
    void write_json(std::ostream&) const;
private:
    using Clock = std::map<QueueId, std::uint64_t>;
    struct Recording { bool closed{}; std::vector<WorkObservation> works; };
    struct Writer { WorkId id{}; Clock clock; AccessEvidence evidence{}; };
    struct Signal { std::uint64_t fence{}, value{}; QueueId owner{}; Clock clock; };
    AttributionLimits limits_;
    std::map<CommandId, Recording> commands_;
    std::map<QueueId, Clock> clocks_;
    std::map<ResourceId, std::vector<Writer>> writers_;
    std::vector<Signal> signals_;
    std::vector<AttributionNode> nodes_;
    std::vector<AttributionEdge> edges_;
    std::uint64_t errors_{};
    bool fail() noexcept { ++errors_; return false; }
    Clock* queue(QueueId);
    WorkId execute(QueueId, CommandId, const WorkObservation&);
};
} // namespace arc
