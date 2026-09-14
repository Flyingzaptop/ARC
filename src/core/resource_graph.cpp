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
        if (!decode(event, payload) || resources_.contains(payload.resource)) {
            return;
        }
        resources_.emplace(payload.resource, ResourceRecord{
            .description = payload,
            .create_timestamp_ns = event.header.timestamp_ns,
            .alive = true,
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
            live_allocation_bytes_ -= it->second.description.allocation_bytes;
        }
        return;
    }
    if (event.header.type == EventType::DescriptorWritten) {
        DescriptorWrittenPayload payload{};
        if (decode(event, payload)) { views_.insert_or_assign(payload.descriptor, ViewRecord{.description = payload}); }
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
        }
        return;
    }
    if (event.header.type == EventType::CopyResource || event.header.type == EventType::CopyBuffer || event.header.type == EventType::CopyTexture) {
        CopyPayload payload{};
        if (decode(event, payload)) {
            for (const auto id : {payload.source, payload.destination}) {
                if (auto resource = resources_.find(id); resource != resources_.end()) {
                    resource->second.last_read_timestamp_ns = event.header.timestamp_ns;
                    resource->second.last_used_frame = presentation_frame_;
                    ++resource->second.read_count;
                }
            }
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
