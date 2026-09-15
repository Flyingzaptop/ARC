#pragma once
#include "arc/ids.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
namespace arc {
using ResidencyId = std::uint64_t;
enum class ResidencyState : std::uint8_t { Unknown, Resident, Evicted, PendingResident, Pinned };
enum class ResidencySafety : std::uint8_t { Unknown, ControlledSafe, Pinned };
enum class PressureState : std::uint8_t { Normal, Pressure, Emergency };
struct ResidencyCost { std::uint64_t bytes{}; double reload_ms{}, reuse_interval{}; };
struct ResidencyObject {
    ResidencyId id{}; ResourceId resource{}; ResidencyState state{ResidencyState::Unknown};
    ResidencySafety safety{ResidencySafety::Unknown}; ResidencyCost cost{};
    std::uint64_t last_use_epoch{}, last_completed_fence{}, use_count{};
};
struct ResidencyAction { enum class Type : std::uint8_t { Evict, MakeResident } type{}; ResidencyId object{}; std::uint64_t bytes{}; };
struct ResidencyMetrics {
    std::uint64_t useful_evictions{}, false_evictions{}, reloads{}, bytes_evicted{}, bytes_made_resident{}, late_residency{};
};
class ResidencyGovernor {
public:
    bool register_object(ResidencyObject object);
    bool note_use(ResidencyId id, std::uint64_t epoch, std::uint64_t completed_fence);
    bool transition(ResidencyId id, ResidencyState expected, ResidencyState next);
    void update_budget(std::uint64_t budget, std::uint64_t usage);
    [[nodiscard]] std::vector<ResidencyAction> plan(std::uint64_t epoch) const;
    [[nodiscard]] std::optional<ResidencyObject> find(ResidencyId id) const;
    [[nodiscard]] PressureState pressure() const noexcept { return pressure_; }
    [[nodiscard]] ResidencyMetrics metrics() const noexcept { return metrics_; }
    void record_eviction(ResidencyId id, bool later_reloaded);
    void record_resident(ResidencyId id, bool late);
private:
    std::unordered_map<ResidencyId, ResidencyObject> objects_;
    PressureState pressure_{PressureState::Normal};
    ResidencyMetrics metrics_{};
    std::uint64_t budget_{}, usage_{};
};
}
