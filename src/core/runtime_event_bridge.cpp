#include "arc/runtime_event_bridge.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace arc {
namespace {

template<class T>
bool decode(const Event& event, T& output) noexcept {
    if (event.header.payload_bytes != sizeof(T)) return false;
    std::memcpy(&output, event.payload.data(), sizeof(T));
    return true;
}

void erase_resource(std::vector<ResourceId>& resources, ResourceId resource) {
    resources.erase(std::remove(resources.begin(), resources.end(), resource), resources.end());
}

}  // namespace

bool RuntimeEventBridge::bind_completion_fence(QueueId queue, std::uint64_t fence_id) noexcept {
    if (!queue || !fence_id) return false;
    const auto it = completion_fence_by_queue_.find(queue);
    if (it != completion_fence_by_queue_.end()) return it->second == fence_id;
    completion_fence_by_queue_.emplace(queue, fence_id);
    return true;
}

bool RuntimeEventBridge::unbind_completion_fence(QueueId queue) noexcept {
    completed_by_queue_.erase(queue);
    return completion_fence_by_queue_.erase(queue) != 0;
}

std::uint64_t RuntimeEventBridge::completion_fence(QueueId queue) const noexcept {
    const auto it = completion_fence_by_queue_.find(queue);
    return it == completion_fence_by_queue_.end() ? 0 : it->second;
}

void RuntimeEventBridge::append_use(CommandId command, ResourceId resource) {
    if (!command || !resource) return;
    auto& uses = command_uses_[command];
    if (uses.empty() || uses.back() != resource) uses.push_back(resource);
}

bool RuntimeEventBridge::queue_submission(QueueId queue, const std::vector<ResourceId>& uses) {
    if (!queue) return false;
    auto& pending_uses = pending_uses_by_queue_[queue];
    pending_uses.insert(pending_uses.end(), uses.begin(), uses.end());

    std::unordered_set<ResourceId> unique;
    std::vector<ResourceId> controlled;
    controlled.reserve(uses.size());
    for (const auto resource : uses) {
        if (!resource || !unique.insert(resource).second || !runtime_.controlled(resource)) continue;
        if (runtime_.inflight_count(resource) == (std::numeric_limits<std::uint32_t>::max)()) return false;
        controlled.push_back(resource);
    }
    for (const auto resource : controlled) {
        if (!runtime_.mark_inflight(resource)) return false;
        ++metrics_.submission_blocks;
    }
    pending_blocks_by_queue_[queue].push_back(std::move(controlled));
    return true;
}

bool RuntimeEventBridge::flush_signal(QueueId queue, std::uint64_t fence_value) {
    if (!queue || !fence_value) return false;
    const auto completed = completed_by_queue_.contains(queue) ? completed_by_queue_.at(queue) : 0;
    bool accepted = true;

    if (auto pending = pending_uses_by_queue_.find(queue); pending != pending_uses_by_queue_.end()) {
        for (const auto resource : pending->second) {
            ++logical_epoch_;
            ++metrics_.resource_uses;
            if (!runtime_.note_use(resource, logical_epoch_, queue, fence_value, completed)) {
                ++metrics_.controller_rejections;
                accepted = false;
            }
        }
        pending->second.clear();
    }

    std::vector<ResourceId> blocked;
    if (auto submissions = pending_blocks_by_queue_.find(queue); submissions != pending_blocks_by_queue_.end()) {
        for (auto& submission : submissions->second) {
            blocked.insert(blocked.end(), submission.begin(), submission.end());
        }
        submissions->second.clear();
    }

    if (!blocked.empty()) {
        if (completed >= fence_value) {
            for (const auto resource : blocked) {
                if (!runtime_.release_inflight(resource, completed)) {
                    ++metrics_.controller_rejections;
                    accepted = false;
                } else {
                    ++metrics_.completion_releases;
                }
            }
        } else {
            signaled_by_queue_[queue].push_back(SignaledBatch{fence_value, std::move(blocked)});
        }
    }
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
        if (!decode(event, payload) || !payload.command || !command_uses_.contains(payload.command)) {
            ++metrics_.malformed_events;
            return false;
        }
        return true;
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
        if (command == command_uses_.end() || !queue_submission(payload.queue, command->second)) {
            ++metrics_.controller_rejections;
            return false;
        }
        ++metrics_.queue_submits;
        return true;
    }
    case EventType::FenceSignal: {
        FencePayload payload{};
        if (!decode(event, payload) || !payload.queue || !payload.fence || !payload.value) {
            ++metrics_.malformed_events;
            return false;
        }
        ++metrics_.fence_signals;
        if (require_explicit_completion_fence_) {
            const auto bound = completion_fence_by_queue_.find(payload.queue);
            if (bound == completion_fence_by_queue_.end()) {
                ++metrics_.unbound_completion_signals;
                return true;
            }
            if (bound->second != payload.fence) {
                ++metrics_.ignored_fence_signals;
                return true;
            }
        }
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
        for (auto& [command, resources] : command_uses_) { (void)command; erase_resource(resources, payload.resource); }
        for (auto& [queue, resources] : pending_uses_by_queue_) { (void)queue; erase_resource(resources, payload.resource); }
        for (auto& [queue, submissions] : pending_blocks_by_queue_) {
            (void)queue;
            for (auto& resources : submissions) erase_resource(resources, payload.resource);
        }
        for (auto& [queue, batches] : signaled_by_queue_) {
            (void)queue;
            for (auto& batch : batches) erase_resource(batch.resources, payload.resource);
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

void RuntimeEventBridge::apply_completion(QueueId queue, std::uint64_t completed_fence) {
    if (!queue) return;
    auto& known = completed_by_queue_[queue];
    if (completed_fence <= known) return;
    known = completed_fence;

    auto batches = signaled_by_queue_.find(queue);
    if (batches != signaled_by_queue_.end()) {
        auto& values = batches->second;
        auto keep = values.begin();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (it->fence_value <= completed_fence) {
                for (const auto resource : it->resources) {
                    if (!runtime_.release_inflight(resource, completed_fence)) ++metrics_.controller_rejections;
                    else ++metrics_.completion_releases;
                }
            } else {
                if (keep != it) *keep = std::move(*it);
                ++keep;
            }
        }
        values.erase(keep, values.end());
    }
    ++metrics_.completion_updates;
}

void RuntimeEventBridge::note_queue_completed(QueueId queue, std::uint64_t completed_fence) {
    if (require_explicit_completion_fence_) return;
    apply_completion(queue, completed_fence);
}

bool RuntimeEventBridge::note_queue_completed(
    QueueId queue,
    std::uint64_t fence_id,
    std::uint64_t completed_fence) {
    if (!queue || !fence_id) return false;
    if (require_explicit_completion_fence_) {
        const auto bound = completion_fence_by_queue_.find(queue);
        if (bound == completion_fence_by_queue_.end() || bound->second != fence_id) return false;
    }
    apply_completion(queue, completed_fence);
    return true;
}

}  // namespace arc
