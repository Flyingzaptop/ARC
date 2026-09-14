#include "arc/clock.hpp"
#include "arc/events.hpp"
#include "arc/trace.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : "example.arcbin";
    arc::Event event{};
    event.header.timestamp_ns = arc::monotonic_time_ns();
    event.header.sequence = 1;
    event.header.type = arc::EventType::SessionStart;

    arc::TraceWriter writer(output);
    if (!writer.append({&event, 1})) {
        std::cerr << "Failed to write trace: " << output << '\n';
        return 1;
    }
    const auto events = arc::TraceReader::read_recoverable(output);
    std::cout << "Wrote and recovered " << events.size() << " event(s): " << output << '\n';
    return events.size() == 1 ? 0 : 1;
}
