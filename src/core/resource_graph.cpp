#include "arc/resource_graph.hpp"

#include <cstring>

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
    }
}

std::optional<ResourceRecord> ResourceGraph::find(const ResourceId id) const {
    const auto it = resources_.find(id);
    return it == resources_.end() ? std::nullopt : std::optional<ResourceRecord>(it->second);
}

std::size_t ResourceGraph::resource_count() const noexcept { return resources_.size(); }
std::uint64_t ResourceGraph::live_allocation_bytes() const noexcept { return live_allocation_bytes_; }

}  // namespace arc
