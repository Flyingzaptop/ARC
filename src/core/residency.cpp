#include "arc/residency.hpp"
#include <algorithm>
namespace arc {
bool ResidencyGovernor::register_object(ResidencyObject object) {
    if (!object.id || objects_.contains(object.id)) { return false; }
    if (object.safety == ResidencySafety::Pinned) { object.state = ResidencyState::Pinned; }
    else if (object.safety == ResidencySafety::Unknown) { object.state = ResidencyState::Unknown; }
    return objects_.emplace(object.id, object).second;
}
bool ResidencyGovernor::note_use(ResidencyId id, std::uint64_t epoch, std::uint64_t fence) {
    auto it = objects_.find(id); if (it == objects_.end()) { return false; }
    auto& object = it->second;
    if (object.use_count && epoch > object.last_use_epoch) {
        const auto interval = static_cast<double>(epoch - object.last_use_epoch);
        object.cost.reuse_interval = object.cost.reuse_interval ? .875 * object.cost.reuse_interval + .125 * interval : interval;
    }
    object.last_use_epoch = epoch; object.last_completed_fence = fence; ++object.use_count; return true;
}
bool ResidencyGovernor::transition(ResidencyId id, ResidencyState expected, ResidencyState next) {
    auto it = objects_.find(id); if (it == objects_.end() || it->second.state != expected) { return false; }
    if (it->second.safety != ResidencySafety::ControlledSafe || expected == ResidencyState::Pinned || expected == ResidencyState::Unknown) { return false; }
    it->second.state = next; return true;
}
void ResidencyGovernor::update_budget(std::uint64_t budget, std::uint64_t usage) {
    budget_ = budget; usage_ = usage;
    if (!budget) { pressure_ = PressureState::Emergency; return; }
    const auto ratio = static_cast<double>(usage) / static_cast<double>(budget);
    if (pressure_ == PressureState::Emergency) { pressure_ = ratio < .75 ? PressureState::Pressure : PressureState::Emergency; }
    else if (ratio >= .95) { pressure_ = PressureState::Emergency; }
    else if (pressure_ == PressureState::Pressure) { pressure_ = ratio < .70 ? PressureState::Normal : PressureState::Pressure; }
    else if (ratio >= .85) { pressure_ = PressureState::Pressure; }
}
std::vector<ResidencyAction> ResidencyGovernor::plan(std::uint64_t epoch) const {
    std::vector<ResidencyAction> actions;
    if (pressure_ == PressureState::Normal) { return actions; }
    for (const auto& [id, object] : objects_) {
        if (object.safety != ResidencySafety::ControlledSafe || object.state != ResidencyState::Resident) { continue; }
        const auto age = epoch >= object.last_use_epoch ? epoch - object.last_use_epoch : 0;
        const auto coldThreshold = static_cast<std::uint64_t>((std::max)(8.0, object.cost.reuse_interval * 2.0));
        if (!object.use_count || age >= coldThreshold) { actions.push_back({ResidencyAction::Type::Evict, id, object.cost.bytes}); }
    }
    std::ranges::sort(actions, std::greater{}, &ResidencyAction::bytes);
    return actions;
}
std::optional<ResidencyObject> ResidencyGovernor::find(ResidencyId id) const {
    const auto it = objects_.find(id); return it == objects_.end() ? std::nullopt : std::optional(it->second);
}
void ResidencyGovernor::record_eviction(ResidencyId id, bool laterReloaded) {
    const auto it = objects_.find(id); if (it == objects_.end()) { return; }
    ++metrics_.useful_evictions; metrics_.bytes_evicted += it->second.cost.bytes;
    if (laterReloaded) { ++metrics_.false_evictions; }
}
void ResidencyGovernor::record_resident(ResidencyId id, bool late) {
    const auto it = objects_.find(id); if (it == objects_.end()) { return; }
    ++metrics_.reloads; metrics_.bytes_made_resident += it->second.cost.bytes; metrics_.late_residency += late;
}
}
