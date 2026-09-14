#include "arc/session.hpp"
#include <array>
namespace arc {
Session::Session(const std::filesystem::path& trace, std::size_t capacity)
    : ring_(capacity), writer_(trace), collector_([this] { collect(); }) {}
Session::~Session() { finish(); }
void Session::finish() {
    if (stopped_) { return; }
    stopping_.store(true, std::memory_order_release);
    collector_.join(); stopped_ = true; graph_.analyze();
}
void Session::collect() {
    try {
        std::array<Event, 256> batch{};
        for (;;) {
            std::size_t count{};
            while (count < batch.size() && ring_.try_pop(batch[count])) { graph_.consume(batch[count]); ++count; }
            if (count) { if (!writer_.append({batch.data(), count})) { io_error_ = true; } }
            else if (stopping_.load(std::memory_order_acquire)) {
                // Recheck after acquire: producer writes precede stop.
                if (!ring_.try_pop(batch[0])) { break; }
                graph_.consume(batch[0]);
                if (!writer_.append({batch.data(), 1})) { io_error_ = true; }
            } else { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        }
    } catch (...) { io_error_ = true; }
}
}
