#include "arc/residency.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string_view>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while (false)

namespace {

struct Result {
    std::uint64_t late{};
    std::uint64_t compulsory{};
    std::uint64_t predictable{};
    std::uint64_t evictions{};
    std::uint64_t promotions{};
    std::uint64_t average_resident{};
    std::uint64_t peak_resident{};
};

Result run_pattern(
    std::string_view name,
    std::uint64_t epochs,
    std::uint64_t warmup,
    unsigned object_count,
    unsigned budget_objects,
    const std::function<unsigned(std::uint64_t)>& requested) {
    using namespace arc;

    constexpr std::uint64_t object_bytes = 100;
    ResidencyPolicyConfig config{};
    config.minimum_residency_age_epochs = 3;
    config.prefetch_horizon_epochs = 8;
    config.post_miss_grace_epochs = 24;
    config.recovery_samples = 2;
    config.pressure_target = .78;
    config.emergency_target = .72;
    config.promotion_ceiling = .86;
    config.minimum_prefetch_confidence = .35;

    ResidencyGovernor governor(config);
    std::vector<bool> resident(object_count + 1, true);
    for (unsigned id = 1; id <= object_count; ++id) {
        const bool ok = governor.register_object({
            .id = id,
            .state = ResidencyState::Resident,
            .safety = ResidencySafety::ControlledSafe,
            .cost = {.bytes = object_bytes, .reload_ms = .25}});
        if (!ok) {
            throw std::runtime_error("registration failed");
        }
    }

    const std::uint64_t budget = static_cast<std::uint64_t>(budget_objects) * object_bytes;
    std::uint64_t resident_bytes = static_cast<std::uint64_t>(object_count) * object_bytes;
    std::uint64_t resident_sum{};
    Result result{};
    result.peak_resident = resident_bytes;

    auto promote = [&](ResidencyId id, bool late, std::uint64_t epoch) {
        if (resident[id]) {
            return;
        }
        if (!governor.require_resident(id)) {
            throw std::runtime_error("demand residency rejected");
        }
        if (!governor.transition(id, ResidencyState::Evicted, ResidencyState::PendingResident) ||
            !governor.transition(id, ResidencyState::PendingResident, ResidencyState::Resident)) {
            throw std::runtime_error("invalid promotion transition");
        }
        resident[id] = true;
        resident_bytes += object_bytes;
        governor.record_resident(id, late, epoch);
        if (!late) {
            ++result.promotions;
        }
    };

    for (std::uint64_t epoch = 1; epoch <= epochs; ++epoch) {
        governor.update_budget(budget, resident_bytes);
        for (const auto& action : governor.plan_evictions(epoch)) {
            if (!resident[action.object]) {
                throw std::runtime_error("duplicate eviction");
            }
            if (!governor.transition(action.object, ResidencyState::Resident, ResidencyState::Evicted)) {
                throw std::runtime_error("invalid eviction transition");
            }
            resident[action.object] = false;
            resident_bytes -= action.bytes;
            governor.record_eviction(action.object, false, epoch);
            ++result.evictions;
        }

        governor.update_budget(budget, resident_bytes);
        for (const auto& action : governor.plan_promotions(epoch)) {
            if (!resident[action.object]) {
                promote(action.object, false, epoch);
            }
        }

        const auto id = requested(epoch);
        if (id == 0 || id > object_count) {
            throw std::runtime_error("pattern returned invalid resource id");
        }
        if (!resident[id]) {
            ++result.late;
            promote(id, true, epoch);
        }
        if (!governor.note_use(id, epoch, 1, epoch, epoch)) {
            throw std::runtime_error("note_use failed");
        }

        result.peak_resident = (std::max)(result.peak_resident, resident_bytes);
        if (epoch > warmup) {
            resident_sum += resident_bytes;
        }
    }

    const auto measured = epochs - warmup;
    result.average_resident = measured ? resident_sum / measured : resident_bytes;
    const auto metrics = governor.metrics();
    result.compulsory = metrics.compulsory_misses;
    result.predictable = metrics.predictable_misses;

    std::cout << name
              << " late=" << result.late
              << " compulsory=" << result.compulsory
              << " predictable=" << result.predictable
              << " evictions=" << result.evictions
              << " promotions=" << result.promotions
              << " avg=" << result.average_resident
              << " peak=" << result.peak_resident
              << '\n';
    return result;
}

}  // namespace

int main() {
    // Stable cyclic reuse should become predictable and avoid demand misses after learning.
    const auto stable = run_pattern("stable-cycle", 4000, 600, 12, 8, [](std::uint64_t epoch) {
        return 1U + static_cast<unsigned>((epoch - 1) % 6);
    });
    CHECK(stable.predictable <= 2);
    CHECK(stable.average_resident <= 800);

    // A phase shift is allowed to cause compulsory misses, but the new phase must settle.
    const auto phased = run_pattern("phase-shift", 4000, 600, 12, 8, [](std::uint64_t epoch) {
        if (epoch < 2000) {
            return 1U + static_cast<unsigned>((epoch - 1) % 4);
        }
        return 5U + static_cast<unsigned>((epoch - 2000) % 4);
    });
    CHECK(phased.predictable <= 4);
    CHECK(phased.average_resident <= 800);

    // Streaming resources have no reusable history. Misses here are compulsory, not predictor failures.
    const auto streaming = run_pattern("streaming", 3200, 400, 32, 12, [](std::uint64_t epoch) {
        return 1U + static_cast<unsigned>(((epoch - 1) / 80) % 32);
    });
    CHECK(streaming.compulsory > 0);
    CHECK(streaming.predictable <= 4);
    CHECK(streaming.average_resident <= 1200);

    // Mixed hot set plus sparse one-off/long-cycle accesses resembles the GPU residency lab.
    const auto mixed = run_pattern("mixed", 5000, 500, 18, 10, [](std::uint64_t epoch) {
        if (epoch % 200 == 150) {
            return 7U + static_cast<unsigned>((epoch / 200) % 10);
        }
        if (epoch % 50 == 25) {
            return 6U;
        }
        if (epoch % 20 == 10) {
            return 5U;
        }
        return 1U + static_cast<unsigned>(epoch % 4);
    });
    CHECK(mixed.compulsory > 0);
    CHECK(mixed.predictable <= 8);
    CHECK(mixed.average_resident <= 1000);

    return 0;
}
