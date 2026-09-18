#include "arc/resource_graph.hpp"

#include <cstring>
#include <algorithm>
#include <utility>

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

void ResourceGraph::erase_view(DescriptorId id) {
    const auto view = views_.find(id);
    if (view == views_.end()) return;
    if (retention_.dead_resources) {
        const auto index = resource_views_.find(view->second.description.resource);
        if (index != resource_views_.end()) {
            index->second.erase(id);
            if (index->second.empty()) resource_views_.erase(index);
        }
    }
    views_.erase(view);
}

void ResourceGraph::assign_view(DescriptorId id, ViewRecord view) {
    erase_view(id);
    if (retention_.dead_resources)
        resource_views_[view.description.resource].insert(id);
    views_.emplace(id, std::move(view));
}

void ResourceGraph::consume(const Event& event) {
    if (event.header.type == EventType::TraceOverflow || event.header.type == EventType::DiagnosticError) { ++errors_; return; }
    std::size_t expected{};
    switch (event.header.type) {
    case EventType::ResourceCreated: expected = sizeof(ResourceCreatePayload); break;
    case EventType::ResourceDestroyed: expected = sizeof(ResourceDestroyPayload); break;
    case EventType::HeapCreated: expected = sizeof(HeapCreatePayload); break;
    case EventType::HeapDestroyed: expected = sizeof(HeapDestroyPayload); break;
    case EventType::DescriptorWritten: expected = sizeof(DescriptorWrittenPayload); break;
    case EventType::DescriptorHeapCreated: case EventType::DescriptorHeapDestroyed: expected = sizeof(DescriptorHeapPayload); break;
    case EventType::DescriptorLocation: expected = sizeof(DescriptorLocationPayload); break;
    case EventType::DescriptorCopied: expected = sizeof(DescriptorCopyPayload); break;
    case EventType::CommandQueueCreated: expected = sizeof(QueueCreatePayload); break;
    case EventType::CommandQueueDestroyed: expected = sizeof(QueueDestroyPayload); break;
    case EventType::CommandListDestroyed: expected = sizeof(CommandDestroyPayload); break;
    case EventType::CommandListCreated: case EventType::CommandListReset: case EventType::CommandListClosed: expected = sizeof(CommandListPayload); break;
    case EventType::QueueSubmit: expected = sizeof(QueueSubmitPayload); break;
    case EventType::CopyResource: case EventType::CopyBuffer: case EventType::CopyTexture: case EventType::ResolveSubresource: expected = sizeof(CopyPayload); break;
    case EventType::Barrier: expected = sizeof(BarrierPayload); break;
    case EventType::ExtendedBarrier: expected = sizeof(ExtendedBarrierPayload); break;
    case EventType::ResourceUse: expected = sizeof(ResourceUsePayload); break;
    case EventType::CommandCounters: case EventType::Draw: case EventType::DrawIndexed: case EventType::Dispatch: case EventType::ExecuteIndirect: expected = sizeof(CountersPayload); break;
    case EventType::FenceSignal: case EventType::FenceWait: expected = sizeof(FencePayload); break;
    case EventType::Present: expected = sizeof(PresentPayload); break;
    case EventType::MemoryBudgetSample: expected = sizeof(MemoryBudgetPayload); break;
    default: break;
    }
    if (event.header.payload_bytes > kMaxEventPayloadBytes || (expected && event.header.payload_bytes != expected)) { ++errors_; return; }
    if (event.header.type == EventType::DescriptorHeapCreated) {
        DescriptorHeapPayload p{}; decode(event, p); descriptor_heaps_[p.heap] = p; return;
    }
    if (event.header.type == EventType::DescriptorHeapDestroyed) {
        DescriptorHeapPayload p{}; decode(event, p);
        if (!descriptor_heaps_.erase(p.heap)) { ++errors_; }
        for (const auto& [id, location] : descriptor_locations_) { if (location.heap == p.heap) { if (auto v = views_.find(id); v != views_.end()) { v->second.alive = false; } } }
        if (retention_.dead_resources) {
            std::erase_if(descriptor_locations_, [&](const auto& entry) {
                if (entry.second.heap != p.heap) return false;
                erase_view(entry.first);
                return true;
            });
        }
        return;
    }
    if (event.header.type == EventType::DescriptorLocation) {
        DescriptorLocationPayload p{}; decode(event, p);
        const auto heap = descriptor_heaps_.find(p.heap);
        if (heap == descriptor_heaps_.end() || p.index >= heap->second.count) { ++errors_; return; }
        descriptor_locations_[p.descriptor] = p; return;
    }
    if (event.header.type == EventType::DescriptorCopied) {
        DescriptorCopyPayload p{}; decode(event, p);
        const auto source = views_.find(p.source);
        if (source == views_.end() || !source->second.alive) { ++errors_; return; }
        auto view = source->second; view.description.descriptor = p.destination;
        assign_view(p.destination, view); return;
    }
    if (event.header.type == EventType::HeapCreated) {
        HeapCreatePayload payload{};
        if (decode(event, payload) && payload.heap && !heaps_.contains(payload.heap)) {
            heaps_.emplace(payload.heap, HeapRecord{.description = payload, .alive = true});
            live_heap_bytes_ += payload.size;
        } else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::HeapDestroyed) {
        HeapDestroyPayload payload{};
        if (!decode(event, payload)) { return; }
        const auto it = heaps_.find(payload.heap);
        if (it != heaps_.end() && it->second.alive) {
            it->second.alive = false;
            live_heap_bytes_ -= it->second.description.size;
            if (retention_.dead_resources) heaps_.erase(it);
        } else { ++errors_; }
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
            if (retention_.dead_resources) {
                dead_resources_.push_back(payload.resource);
                while (dead_resources_.size() > retention_.dead_resources) {
                    const auto expired = dead_resources_.front();
                    dead_resources_.pop_front();
                    last_pruned_resource_usage_frame_ = std::max(
                        last_pruned_resource_usage_frame_, resources_.at(expired).last_used_frame);
                    resources_.erase(expired);
                    if (const auto index = resource_views_.find(expired); index != resource_views_.end()) {
                        for (const auto descriptor : index->second) views_.erase(descriptor);
                        resource_views_.erase(index);
                    }
                    ++resource_history_generation_;
                }
            }
        } else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::DescriptorWritten) {
        DescriptorWrittenPayload payload{};
        if (decode(event, payload)) {
            if (payload.type != ViewType::Sampler && (!payload.resource || !resources_.contains(payload.resource))) { ++errors_; return; }
            if (payload.type == ViewType::Sampler && payload.resource) { ++errors_; return; }
            assign_view(payload.descriptor, ViewRecord{.description = payload});
            if (auto r = resources_.find(payload.resource); r != resources_.end()) {
                r->second.evidence |= 1U << static_cast<unsigned>(payload.type);
            }
        }
        return;
    }
    if (event.header.type == EventType::CommandQueueCreated) {
        QueueCreatePayload payload{};
        if (decode(event, payload) && payload.queue && !queues_.contains(payload.queue)) { queues_.emplace(payload.queue, QueueRecord{.description = payload}); }
        else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::CommandListDestroyed) {
        CommandDestroyPayload p{}; decode(event, p);
        if (!commands_.erase(p.command)) ++errors_;
        return;
    }
    if (event.header.type == EventType::CommandQueueDestroyed) {
        QueueDestroyPayload p{}; decode(event, p);
        auto q = queues_.find(p.queue);
        if (q == queues_.end() || !q->second.alive) { ++errors_; return; }
        if (retention_.dead_resources) {
            queues_.erase(q);
            bool lost_evidence = false;
            for (auto& [id, resource] : resources_) {
                (void)id;
                if (resource.queues.erase(p.queue)) {
                    resource.queue_use_counts.erase(p.queue);
                    last_pruned_resource_usage_frame_ = std::max(last_pruned_resource_usage_frame_, resource.last_used_frame);
                    lost_evidence = true;
                }
            }
            if (lost_evidence) ++resource_history_generation_;
        } else q->second.alive = false;
        return;
    }
    if (event.header.type == EventType::QueueSubmit) {
        QueueSubmitPayload payload{};
        if (decode(event, payload)) {
            bool valid_queue=false;
            if (auto queue = queues_.find(payload.queue); queue != queues_.end() && queue->second.alive) { ++queue->second.submissions; valid_queue=true; }
            else { ++errors_; }
            auto command = commands_.find(payload.command);
            if (command == commands_.end() || !command->second.closed) { ++errors_; return; }
            if (!valid_queue) return;
            submissions_.push_back({payload, event.header.timestamp_ns, presentation_frame_, command->second.counters});
            ++totals_.submissions;
            const auto& counters = command->second.counters;
            totals_.counters.draws += counters.draws;
            totals_.counters.indexed_draws += counters.indexed_draws;
            totals_.counters.dispatches += counters.dispatches;
            totals_.counters.indirect += counters.indirect;
            totals_.counters.draw_items += counters.draw_items;
            totals_.counters.dispatch_groups += counters.dispatch_groups;
            if (retention_.submissions && submissions_.size() > retention_.submissions) {
                last_pruned_submission_frame_ = submissions_.front().presentation;
                submissions_.pop_front();
            }
            for (const auto& copy : command->second.copies) {
                use(copy.source, payload.queue, event.header.timestamp_ns, false);
                use(copy.destination, payload.queue, event.header.timestamp_ns, true);
                copies_.push_back({copy, payload.queue, event.header.timestamp_ns, presentation_frame_});
                ++totals_.copies;
                if (retention_.copies && copies_.size() > retention_.copies) {
                    last_pruned_copy_frame_ = copies_.front().presentation;
                    copies_.pop_front();
                }
            }
            for (const auto& u : command->second.uses) { use(u.resource, payload.queue, event.header.timestamp_ns, u.write != 0); }
        }
        return;
    }
    if (event.header.type == EventType::CommandListCreated || event.header.type == EventType::CommandListReset || event.header.type == EventType::CommandListClosed) {
        CommandListPayload p{};
        if (decode(event, p)) {
            const auto existing = commands_.find(p.command);
            if (event.header.type == EventType::CommandListCreated) {
                if (!p.command || existing != commands_.end()) { ++errors_; }
                else { commands_.emplace(p.command, CommandRecord{}); }
            } else if (existing == commands_.end()) { ++errors_; }
            else if (event.header.type == EventType::CommandListClosed) {
                if (existing->second.closed) { ++errors_; } else { existing->second.closed = true; }
            } else { existing->second = {}; }
        }
        return;
    }
    if (event.header.type == EventType::Barrier) {
        BarrierPayload p{};
        if (!decode(event, p)) { ++errors_; return; }
        const auto command = commands_.find(p.command);
        if (command != commands_.end() && resources_.contains(p.resource)) { command->second.barriers.push_back(p); }
        else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::ExtendedBarrier) {
        ExtendedBarrierPayload p{};
        if (!decode(event, p)) { ++errors_; return; }
        const auto command = commands_.find(p.command);
        if (command != commands_.end() && (!p.resource || resources_.contains(p.resource))) { command->second.extended_barriers.push_back(p); }
        else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::ResourceUse) {
        ResourceUsePayload p{};
        if (!decode(event, p)) { ++errors_; return; }
        const auto command = commands_.find(p.command);
        if (command != commands_.end() && resources_.contains(p.resource)) { command->second.uses.push_back(p); }
        else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::CommandCounters) {
        CountersPayload p{};
        if (!decode(event, p)) { ++errors_; return; }
        const auto command = commands_.find(p.command);
        if (command != commands_.end()) { command->second.counters = p; }
        else { ++errors_; }
        return;
    }
    if (event.header.type == EventType::CopyResource || event.header.type == EventType::CopyBuffer || event.header.type == EventType::CopyTexture || event.header.type == EventType::ResolveSubresource) {
        CopyPayload payload{};
        if (decode(event, payload)) {
            const auto command = commands_.find(payload.command);
            if (command == commands_.end() || !resources_.contains(payload.source) || !resources_.contains(payload.destination)) { ++errors_; }
            else { command->second.copies.push_back(payload); }
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
    ++r.queue_use_counts[queue];
    if (r.read_count + r.write_count && presentation_frame_ > r.last_used_frame) {
        const auto gap_frames = presentation_frame_ - r.last_used_frame;
        const auto gap = static_cast<double>(gap_frames);
        r.reuse_interval_frames = r.reuse_interval_frames == 0 ? gap : 0.875 * r.reuse_interval_frames + 0.125 * gap;
        r.reuse_gap_frames_sum += gap_frames;
        ++r.reuse_gap_samples;
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
