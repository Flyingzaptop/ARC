#include "arc/residency.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <unordered_set>
#include <vector>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " seed=" << seed << " epoch=" << epoch << " line=" << __LINE__ << '\n'; return 1; } } while(false)
int main() {
    using namespace arc;
    constexpr std::uint64_t objectBytes = 64;
    constexpr unsigned objectCount = 32;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        ResidencyPolicyConfig config{};
        config.minimum_residency_age_epochs = 2;
        config.prefetch_horizon_epochs = 5;
        config.recovery_samples = 2;
        ResidencyGovernor governor(config);
        std::array<bool, objectCount + 1> controlled{};
        std::array<bool, objectCount + 1> resident{};
        std::uint64_t managedUsage{};
        for (unsigned id = 1; id <= objectCount; ++id) {
            const auto safety = id % 13 == 0 ? ResidencySafety::Pinned : id % 11 == 0 ? ResidencySafety::Unknown : ResidencySafety::ControlledSafe;
            controlled[id] = safety == ResidencySafety::ControlledSafe;
            resident[id] = true;
            CHECK(governor.register_object({.id=id,.state=ResidencyState::Resident,.safety=safety,.cost={.bytes=objectBytes,.reload_ms=.1 + static_cast<double>(id % 7) * .05}}));
            managedUsage += objectBytes;
        }
        std::uint64_t random = seed * 0x9e3779b97f4a7c15ULL;
        auto next = [&] {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random;
        };
        for (std::uint64_t epoch = 1; epoch <= 4000; ++epoch) {
            const auto budget = epoch % 997 == 0 ? 0ULL : 1152ULL + ((epoch / 137) % 4) * 128ULL;
            governor.update_budget(budget, managedUsage);
            std::unordered_set<ResidencyId> selected;
            for (const auto& action : governor.plan_evictions(epoch)) {
                CHECK(action.type == ResidencyAction::Type::Evict);
                CHECK(action.object >= 1 && action.object <= objectCount);
                CHECK(controlled[action.object]);
                CHECK(resident[action.object]);
                CHECK(selected.insert(action.object).second);
                const auto before = governor.find(action.object); CHECK(before.has_value());
                CHECK(action.required_fence == before->last_use_fence);
                CHECK(governor.transition(action.object, ResidencyState::Resident, ResidencyState::Evicted));
                resident[action.object] = false; managedUsage -= action.bytes;
            }
            governor.update_budget(budget, managedUsage);
            selected.clear();
            for (const auto& action : governor.plan_promotions(epoch)) {
                CHECK(governor.pressure() == PressureState::Normal);
                CHECK(action.type == ResidencyAction::Type::MakeResident);
                CHECK(controlled[action.object]);
                CHECK(!resident[action.object]);
                CHECK(selected.insert(action.object).second);
                CHECK(governor.transition(action.object, ResidencyState::Evicted, ResidencyState::PendingResident));
                CHECK(governor.transition(action.object, ResidencyState::PendingResident, ResidencyState::Resident));
                resident[action.object] = true; managedUsage += action.bytes;
            }
            unsigned request = 1 + static_cast<unsigned>(next() % objectCount);
            while (!controlled[request]) request = 1 + static_cast<unsigned>(next() % objectCount);
            if (!resident[request]) {
                const auto demand = governor.require_resident(request); CHECK(demand.has_value());
                CHECK(governor.transition(request, ResidencyState::Evicted, ResidencyState::PendingResident));
                CHECK(governor.transition(request, ResidencyState::PendingResident, ResidencyState::Resident));
                resident[request] = true; managedUsage += demand->bytes;
            }
            const auto fence = epoch + seed * 10000;
            CHECK(governor.note_use(request, epoch, 1, fence, fence));
            for (unsigned id = 1; id <= objectCount; ++id) {
                const auto object = governor.find(id); CHECK(object.has_value());
                if (!controlled[id]) {
                    CHECK(object->state == (id % 13 == 0 ? ResidencyState::Pinned : ResidencyState::Unknown));
                    CHECK(resident[id]);
                } else {
                    CHECK((resident[id] && object->state == ResidencyState::Resident) || (!resident[id] && object->state == ResidencyState::Evicted));
                }
            }
        }
    }
    std::cout << "residency invariant stress passed\n";
    return 0;
}
