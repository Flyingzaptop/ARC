#pragma once
#include "arc/statistics.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>
namespace calibration {
inline volatile std::uint64_t sink{};
template<class F> arc::Statistics measure(F fn) {
    fn(); std::vector<double> values;
    for (unsigned run = 0; run < 9; ++run) {
        auto start = std::chrono::steady_clock::now(); fn();
        values.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return arc::summarize(values);
}
inline std::uint64_t cpu(std::uint64_t seed) {
    for (unsigned i = 0; i < 2000000; ++i) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; }
    return seed;
}
inline std::string run(const std::filesystem::path& file) {
    std::ostringstream json;
    bool first = true;
    auto record = [&](const char* name, auto fn) {
        const auto s = measure(fn);
        if (!first) { json << ','; } first = false;
        json << '"' << name << "\":{\"median_ms\":" << s.median << ",\"p10_ms\":" << s.p10 << ",\"p90_ms\":" << s.p90 << ",\"variance_ms2\":" << s.variance << '}';
    };
    const auto threads = (std::min)(8U, (std::max)(1U, std::thread::hardware_concurrency()));
    record("cpu_single", [] { sink = cpu(123); });
    record("cpu_multi", [&] {
        std::vector<std::uint64_t> results(threads); std::vector<std::thread> workers;
        for (unsigned t = 0; t < threads; ++t) { workers.emplace_back([&, t] { results[t] = cpu(t + 1); }); }
        for (auto& w : workers) { w.join(); } sink = results[0];
    });
    constexpr std::size_t size = 32 * 1024 * 1024;
    std::vector<char> source(size, 17), target(size);
    record("ram_copy_32MiB", [&] { std::memcpy(target.data(), source.data(), size); sink = target[size / 2]; });
    record("ram_multi_copy_32MiB", [&] {
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < threads; ++t) { workers.emplace_back([&, t] { auto begin = size * t / threads, end = size * (t + 1) / threads; std::memcpy(target.data() + begin, source.data() + begin, end - begin); }); }
        for (auto& w : workers) { w.join(); } sink = target[size / 2];
    });
    std::vector<std::uint32_t> chain(1 << 20);
    for (std::uint32_t i = 0; i < chain.size(); ++i) { chain[i] = (i + 104729) & ((1 << 20) - 1); }
    record("ram_pointer_chase_2M", [&] { std::uint32_t index{}; for (unsigned i = 0; i < 2000000; ++i) { index = chain[index]; } sink = index; });
    const auto fileSize = std::filesystem::file_size(file);
    if (!fileSize) { throw std::runtime_error("empty storage input"); }
    std::ifstream input(file, std::ios::binary);
    const auto count = static_cast<std::streamsize>((std::min)(std::uintmax_t{size}, fileSize));
    record("storage_cached_sequential", [&] {
        input.clear(); input.seekg(0); input.read(target.data(), count);
        if (input.gcount() != count) { throw std::runtime_error("short storage read"); } sink = target[0];
    });
    record("storage_cached_small_reads_64", [&] {
        for (unsigned i = 0; i < 64; ++i) {
            const auto block = (std::min)(std::uintmax_t{4096}, fileSize);
            input.clear(); input.seekg((i * 104729ULL) % (fileSize - block + 1)); input.read(target.data(), static_cast<std::streamsize>(block));
            if (input.gcount() != static_cast<std::streamsize>(block)) { throw std::runtime_error("short storage read"); }
        } sink = target[0];
    });
    return "\"warmup_runs\":1,\"measured_runs\":9,\"benchmark_threads\":" + std::to_string(threads) + ",\"storage_bytes_per_sequential_run\":" + std::to_string(count) + ",\"benchmarks\":{" + json.str() + "}";
}
}
