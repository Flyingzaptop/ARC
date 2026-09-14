#include "arc/resource_graph.hpp"

#include <cstring>
#include <algorithm>

namespace arc {
namespace {

template <typename T>
bool decode(const Event& event, T& output) noexcept {
    if (event.header.payload_bytes != sizeof(T)) {
        return false;
    }
    std::memcpy(&output, event.payload.data(), sizeof(T));
    return true;
}

}  // namespace

void ResourceGraph::consume(const Event& event) {
    if (event.header.type == EventType::TraceOverflow || event.header.type == EventType::DiagnosticError) { ++errors_; return; }
    std::size_t expected{};
    switch (event.header.type) {
    case EventType::ResourceCreated: expected = sizeof(ResourceCreatePayload); break;
    case EventType::ResourceDestroyed: expected = sizeof(ResourceDestroyPayload); break;
    case EventType::HeapCreated: expected = sizeof(HeapCreatePayload); break;
    case EventType::HeapDestroyed: expected = sizeof(HeapDestroyPayload); break;
    case EventType::DescriptorWritten: expected = sizeof(DescriptorWrittenPayload); break;
    case EventType::CommandQueueCreated: expected = sizeof(QueueCreatePayload); break;
    case EventType::CommandListCreated: case EventType::CommandListReset: case EventType::CommandListClosed: expected = sizeof(CommandListPayload); break;
    case EventType::QueueSubmit: expected = sizeof(QueueSubmitPayload); break;
    case EventType::CopyResource: case EventType::CopyBuffer: case EventType::CopyTexture: case EventType::ResolveSubresource: expected = sizeof(CopyPayload); break;
    case EventType::Barrier: expected = sizeof(BarrierPayload); break;
    case EventType::Present: expected = sizeof(PresentPayload); break;
    case EventType::MemoryBudgetSample: expected = sizeof(MemoryBudgetPayload); break;
    default: break;
    }
    if (event.header.payload_bytes > kMaxEventPayloadBytes || (expected && event.header.payload_bytes != expected)) { ++errors_; return; }
    if (event.header.type == EventType::HeapCreated) {
        HeapCreatePayload payload{};
        if (decode(event, payload) && !heaps_.contains(payload.heap)) {
            heaps_.emplace(payload.heap, HeapRecord{.description = payload, .alive = true});
            live_heap_bytes_ += payload.size;
        }
        return;
    }
    if (event.header.type == EventType::HeapDestroyed) {
        HeapDestroyPayload payload{};
        if (!decode(event, payload)) { return; }
        const auto it = heaps_.find(payload.heap);
        if (it != heaps_.end() && it->second.alive) {
            it->second.alive = false;
            live_heap_bytes_ -= it->second.description.size;
        }
        return;
    }
    if (event.header.type == EventType::ResourceCreated) {
        ResourceCreatePayload payload{};
        if (!decode(event, payload) || !payload.resource || resources_.contains(payload.resource)) {
            ++errors_;
            return;
        }
        resources_.emplace(payload.resource, ResourceRecord{
            .description = payload,
            .create_timestamp_ns = event.header.timestamp_ns,
            .alive = true,
            .create_sequence = event.header.sequence,
        });
        live_allocation_bytes_ += payload.allocation_bytes;
        return;
    }
    if (event.header.type == EventType::ResourceDestroyed) {
        ResourceDestroyPayload payload{};
        if (!decode(event, payload)) {
            return;
        }
        const auto it = resources_.find(payload.resource);
        if (it != resources_.end() && it->second.alive) {
            it->second.alive = false;
            it->second.destroy_timestamp_ns = event.header.timestamp_ns;
            it->second.destroy_sequence = event.header.sequence;
            live_allocation_bytes_ -= it->second.description.allocation_bytes;
        } else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::DescriptorWritten) {
        DescriptorWrittenPayload payload{};
        if (decode(event, payload)) {
            views_.insert_or_assign(payload.descriptor, ViewRecord{.description = payload});
            if (auto r = resources_.find(payload.resource); r != resources_.end()) {
                r->second.evidence |= 1U << static_cast<unsigned>(payload.type);
            }
        }
        return;
    }
    if (event.header.type == EventType::CommandQueueCreated) {
        QueueCreatePayload payload{};
        if (decode(event, payload)) { queues_.insert_or_assign(payload.queue, QueueRecord{.description = payload}); }
        return;
    }
    if (event.header.type == EventType::QueueSubmit) {
        QueueSubmitPayload payload{};
        if (decode(event, payload)) {
            if (auto queue = queues_.find(payload.queue); queue != queues_.end()) { ++queue->second.submissions; }
            else { ++errors_; }
            auto command = commands_.find(payload.command);
            if (command == commands_.end() || !command->second.closed) { ++errors_; return; }
            submissions_.push_back({payload, event.header.timestamp_ns, presentation_frame_, command->second.counters});
            for (const auto& copy : command->second.copies) {
                use(copy.source, payload.queue, event.header.timestamp_ns, false);
                use(copy.destination, payload.queue, event.header.timestamp_ns, true);
                copies_.push_back({copy, payload.queue, event.header.timestamp_ns});
            }
            for (const auto& u : command->second.uses) { use(u.resource, payload.queue, event.header.timestamp_ns, u.write != 0); }
        }
        return;
    }
    if (event.header.type == EventType::CommandListCreated || event.header.type == EventType::CommandListReset || event.header.type == EventType::CommandListClosed) {
        CommandListPayload p{};
        if (decode(event, p)) {
            if (event.header.type == EventType::CommandListClosed) { commands_[p.command].closed = true; }
            else { commands_[p.command] = {}; }
        }
        return;
    }
    if (event.header.type == EventType::Barrier) {
        BarrierPayload p{};
        if (decode(event, p)) { commands_[p.command].barriers.push_back(p); }
        return;
    }
    if (event.header.type == EventType::ResourceUse) {
        ResourceUsePayload p{};
        if (decode(event, p)) { commands_[p.command].uses.push_back(p); }
        return;
    }
    if (event.header.type == EventType::CommandCounters) {
        CountersPayload p{};
        if (decode(event, p)) { commands_[p.command].counters = p; }
        return;
    }
    if (event.header.type == EventType::CopyResource || event.header.type == EventType::CopyBuffer || event.header.type == EventType::CopyTexture || event.header.type == EventType::ResolveSubresource) {
        CopyPayload payload{};
        if (decode(event, payload)) {
            commands_[payload.command].copies.push_back(payload);
        }
        return;
    }
    if (event.header.type == EventType::Present) {
        PresentPayload payload{};
        if (decode(event, payload)) { presentation_frame_ = payload.frame; }
        return;
    }
    if (event.header.type == EventType::MemoryBudgetSample) {
        MemoryBudgetPayload payload{};
        if (decode(event, payload)) { latest_budget_ = payload; }
    }
}

void ResourceGraph::use(ResourceId id, QueueId queue, std::uint64_t timestamp, bool write) {
    auto it = resources_.find(id);
    if (it == resources_.end() || !it->second.alive) { ++errors_; return; }
    auto& r = it->second;
    r.queues.insert(queue);
    if (r.read_count + r.write_count && presentation_frame_ > r.last_used_frame) {
        const auto gap = static_cast<double>(presentation_frame_ - r.last_used_frame);
        r.reuse_interval_frames = r.reuse_interval_frames == 0 ? gap : 0.875 * r.reuse_interval_frames + 0.125 * gap;
        if (gap > 1) { ++r.usage_bursts; }
    }
    r.last_used_frame = presentation_frame_;
    if (write) { ++r.write_count; r.last_write_timestamp_ns = timestamp; }
    else { ++r.read_count; r.last_read_timestamp_ns = timestamp; }
}

std::uint64_t ResourceGraph::committed_bytes() const noexcept {
    std::uint64_t sum{};
    for (const auto& [id, r] : resources_) {
        if (r.alive && r.description.allocation_kind == ResourceAllocationKind::Committed) { sum += r.description.allocation_bytes; }
    }
    return sum;
}

std::vector<ResourceId> ResourceGraph::alive_at(std::uint64_t timestamp) const {
    std::vector<ResourceId> result;
    for (const auto& [id, r] : resources_) {
        if (r.create_timestamp_ns <= timestamp && (r.alive || timestamp < r.destroy_timestamp_ns)) { result.push_back(id); }
    }
    return result;
}

void ResourceGraph::analyze() {
    constexpr auto srv = 1U << static_cast<unsigned>(ViewType::Srv);
    constexpr auto uav = 1U << static_cast<unsigned>(ViewType::Uav);
    constexpr auto targets = (1U << static_cast<unsigned>(ViewType::Rtv)) | (1U << static_cast<unsigned>(ViewType::Dsv));
    for (auto& [id, r] : resources_) {
        r.safety = SafetyClass::Unknown; r.safety_confidence = 0;
        if (r.evidence & uav) { r.safety = SafetyClass::Red; r.safety_confidence = 1; }
        else if (r.evidence & targets) { r.safety = SafetyClass::Yellow; r.safety_confidence = 1; }
        else if ((r.evidence & srv) && r.description.kind == ResourceKind::Texture2D && r.description.mip_levels > 1) {
            r.safety = SafetyClass::GreenCandidate; r.safety_confidence = 0.6F;
        }
        if (r.safety == SafetyClass::Red) { r.temperature = Temperature::Pinned; }
        else if (r.read_count + r.write_count == 0) { r.temperature = Temperature::Unknown; }
        else {
            const auto age = presentation_frame_ >= r.last_used_frame ? presentation_frame_ - r.last_used_frame : 0;
            const auto warmHorizon = (std::max)(60.0, r.reuse_interval_frames * 3);
            r.temperature = age <= 3 ? Temperature::Hot : static_cast<double>(age) <= warmHorizon ? Temperature::Warm : Temperature::Cold;
        }
    }
}

std::vector<ResourceId> ResourceGraph::with_view(ViewType type) const {
    std::vector<ResourceId> result;
    for (const auto& [id, r] : resources_) { if (r.evidence & (1U << static_cast<unsigned>(type))) { result.push_back(id); } }
    return result;
}
std::vector<ResourceId> ResourceGraph::unused_for(FrameId presentations) const {
    std::vector<ResourceId> result;
    for (const auto& [id, r] : resources_) {
        if (r.alive && r.read_count + r.write_count && presentation_frame_ >= r.last_used_frame && presentation_frame_ - r.last_used_frame >= presentations) { result.push_back(id); }
    }
    return result;
}
std::vector<ResourceId> ResourceGraph::seen_on_queue(QueueId queue) const {
    std::vector<ResourceId> result;
    for (const auto& [id, r] : resources_) { if (r.queues.contains(queue)) { result.push_back(id); } }
    return result;
}
std::vector<ResourceId> ResourceGraph::largest_textures(std::size_t limit) const {
    auto sorted = largest_resources(resources_.size()); std::vector<ResourceId> result;
    for (auto id : sorted) {
        const auto kind = resources_.at(id).description.kind;
        if (kind == ResourceKind::Texture1D || kind == ResourceKind::Texture2D || kind == ResourceKind::Texture3D) { result.push_back(id); }
        if (result.size() >= limit) { break; }
    }
    if (!limit) { result.clear(); }
    return result;
}

std::optional<ResourceRecord> ResourceGraph::find(const ResourceId id) const {
    const auto it = resources_.find(id);
    return it == resources_.end() ? std::nullopt : std::optional<ResourceRecord>(it->second);
}

std::optional<HeapRecord> ResourceGraph::find_heap(const HeapId id) const {
    const auto it = heaps_.find(id);
    return it == heaps_.end() ? std::nullopt : std::optional<HeapRecord>(it->second);
}

std::optional<ViewRecord> ResourceGraph::find_view(const DescriptorId id) const {
    const auto it = views_.find(id);
    return it == views_.end() ? std::nullopt : std::optional<ViewRecord>(it->second);
}

std::size_t ResourceGraph::resource_count() const noexcept { return resources_.size(); }
std::uint64_t ResourceGraph::live_allocation_bytes() const noexcept { return live_allocation_bytes_; }
std::uint64_t ResourceGraph::live_heap_bytes() const noexcept { return live_heap_bytes_; }
FrameId ResourceGraph::presentation_frame() const noexcept { return presentation_frame_; }
std::optional<MemoryBudgetPayload> ResourceGraph::latest_budget() const noexcept { return latest_budget_; }

std::vector<ResourceId> ResourceGraph::resources_on_heap(const HeapId heap) const {
    std::vector<ResourceId> result;
    for (const auto& [id, resource] : resources_) { if (resource.description.heap == heap) { result.push_back(id); } }
    return result;
}

std::vector<ResourceId> ResourceGraph::largest_resources(const std::size_t limit) const {
    std::vector<std::pair<ResourceId, std::uint64_t>> sorted;
    for (const auto& [id, resource] : resources_) { sorted.emplace_back(id, resource.description.allocation_bytes); }
    std::ranges::sort(sorted, {}, &std::pair<ResourceId, std::uint64_t>::second);
    std::reverse(sorted.begin(), sorted.end());
    std::vector<ResourceId> result;
    const auto count = std::min(limit, sorted.size());
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) { result.push_back(sorted[index].first); }
    return result;
}

}  // namespace arc
