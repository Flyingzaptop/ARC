#include "arc/runtime_event_bridge.hpp"

#include <cstring>

namespace arc {
namespace {

template<class T>
bool decode(const Event& event, T& output) noexcept {
    if (event.header.payload_bytes != sizeof(T)) return false;
    std::memcpy(&output, event.payload.data(), sizeof(T));
    return true;
}

}  // namespace

void RuntimeEventBridge::append_use(CommandId command, ResourceId resource) {
    if (!command || !resource) return;
    auto& uses = command_uses_[command];
    if (uses.empty() || uses.back() != resource) uses.push_back(resource);
}

bool RuntimeEventBridge::flush_signal(QueueId queue, std::uint64_t fence_value) {
    if (!queue || !fence_value) return false;
    auto pending = pending_by_queue_.find(queue);
    if (pending == pending_by_queue_.end()) return true;
    const auto completed = completed_by_queue_.contains(queue) ? completed_by_queue_.at(queue) : 0;
    bool accepted = true;
    for (const auto resource : pending->second) {
        ++logical_epoch_;
        ++metrics_.resource_uses;
        seen_by_queue_[queue].insert(resource);
        if (!runtime_.note_use(resource, logical_epoch_, queue, fence_value, completed)) {
            ++metrics_.controller_rejections;
            accepted = false;
        }
    }
    pending->second.clear();
    return accepted;
}

bool RuntimeEventBridge::consume(const Event& event) {
    ++metrics_.events;
    if (event.header.payload_bytes > kMaxEventPayloadBytes) {
        ++metrics_.malformed_events;
        return false;
    }

    switch (event.header.type) {
    case EventType::CommandListCreated:
    case EventType::CommandListReset: {
        CommandListPayload payload{};
        if (!decode(event, payload) || !payload.command) { ++metrics_.malformed_events; return false; }
        command_uses_[payload.command].clear();
        return true;
    }
    case EventType::CommandListClosed: {
        CommandListPayload payload{};
        if (!decode(event, payload) || !payload.command) { ++metrics_.malformed_events; return false; }
        return command_uses_.contains(payload.command);
    }
    case EventType::ResourceUse: {
        ResourceUsePayload payload{};
        if (!decode(event, payload) || !payload.command || !payload.resource) { ++metrics_.malformed_events; return false; }
        append_use(payload.command, payload.resource);
        return true;
    }
    case EventType::CopyResource:
    case EventType::CopyBuffer:
    case EventType::CopyTexture:
    case EventType::ResolveSubresource: {
        CopyPayload payload{};
        if (!decode(event, payload) || !payload.command || !payload.source || !payload.destination) {
            ++metrics_.malformed_events;
            return false;
        }
        append_use(payload.command, payload.source);
        append_use(payload.command, payload.destination);
        return true;
    }
    case EventType::QueueSubmit: {
        QueueSubmitPayload payload{};
        if (!decode(event, payload) || !payload.queue || !payload.command) { ++metrics_.malformed_events; return false; }
        const auto command = command_uses_.find(payload.command);
        if (command == command_uses_.end()) { ++metrics_.malformed_events; return false; }
        auto& pending = pending_by_queue_[payload.queue];
        pending.insert(pending.end(), command->second.begin(), command->second.end());
        ++metrics_.queue_submits;
        return true;
    }
    case EventType::FenceSignal: {
        FencePayload payload{};
        if (!decode(event, payload) || !payload.queue || !payload.value) { ++metrics_.malformed_events; return false; }
        ++metrics_.fence_signals;
        return flush_signal(payload.queue, payload.value);
    }
    case EventType::MemoryBudgetSample: {
        MemoryBudgetPayload payload{};
        if (!decode(event, payload)) { ++metrics_.malformed_events; return false; }
        runtime_.update_budget(payload);
        ++metrics_.budget_samples;
        return true;
    }
    case EventType::ResourceDestroyed: {
        ResourceDestroyPayload payload{};
        if (!decode(event, payload) || !payload.resource) { ++metrics_.malformed_events; return false; }
        runtime_.unregister_resource(payload.resource);
        for (auto& [command, resources] : command_uses_) {
            (void)command;
            resources.erase(std::remove(resources.begin(), resources.end(), payload.resource), resources.end());
        }
        for (auto& [queue, resources] : pending_by_queue_) {
            (void)queue;
            resources.erase(std::remove(resources.begin(), resources.end(), payload.resource), resources.end());
        }
        for (auto& [queue, resources] : seen_by_queue_) {
            (void)queue;
            resources.erase(payload.resource);
        }
        ++metrics_.resources_destroyed;
        return true;
    }
    case EventType::Present: {
        PresentPayload payload{};
        if (!decode(event, payload)) { ++metrics_.malformed_events; return false; }
        presentation_frame_ = payload.frame ? payload.frame : presentation_frame_ + 1;
        return true;
    }
    default:
        return true;
    }
}

void RuntimeEventBridge::note_queue_completed(QueueId queue, std::uint64_t completed_fence) {
    if (!queue) return;
    auto& known = completed_by_queue_[queue];
    if (completed_fence <= known) return;
    known = completed_fence;
    const auto resources = seen_by_queue_.find(queue);
    if (resources != seen_by_queue_.end()) {
        for (const auto resource : resources->second) {
            (void)runtime_.note_completed(resource, completed_fence);
        }
    }
    ++metrics_.completion_updates;
}

}  // namespace arc
