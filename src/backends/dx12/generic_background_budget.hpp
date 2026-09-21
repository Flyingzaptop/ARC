#pragma once
#include <mutex>
namespace arc::dx12 {
// Only background compiler/critic callers acquire this. Never on render threads.
inline std::timed_mutex& background_compute_gate(){static std::timed_mutex gate;return gate;}
}
