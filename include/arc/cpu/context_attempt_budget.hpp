#pragma once
#include <cmath>
namespace arc::cpu {
struct ContextAttemptBudget {
    double total_us{};
    bool enabled{true};
    bool account(double elapsed_us) noexcept {
        if (!std::isfinite(elapsed_us) || elapsed_us < 0) return enabled=false;
        total_us += elapsed_us;
        enabled = enabled && elapsed_us <= 5000 && total_us <= 50000;
        return enabled;
    }
};
}
