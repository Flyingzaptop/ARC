#pragma once
#include <cstdint>
#include <ostream>
#include <vector>

namespace arc {
// CPU timestamps at successful Present return. This measures submission cadence,
// not displayed FPS, GPU busy time, or an image-quality-controlled speedup.
class FrameTimingWindow {
public:
    explicit FrameTimingWindow(std::uint64_t duration_ns=60'000'000'000ULL,
                               std::size_t capacity=65536);
    void present(std::uint64_t timestamp_ns,std::uint64_t swapchain,bool visible_success);
    void expire() noexcept;
    [[nodiscard]] bool done()const noexcept {return reason_!=Reason::Pending;}
    [[nodiscard]] bool valid()const noexcept {return reason_==Reason::Complete;}
    [[nodiscard]] std::uint64_t swapchain()const noexcept{return swapchain_;}
    [[nodiscard]] std::size_t intervals()const noexcept{return intervals_.size();}
    [[nodiscard]] double mean_fps()const noexcept;
    void write_json(std::ostream&)const;
private:
    enum class Reason {Pending,Complete,NoPresents,Interrupted,InvalidTimestamp,Capacity};
    Reason reason_{Reason::Pending};
    std::uint64_t duration_,first_{},last_{},swapchain_{},ignored_{};
    std::size_t capacity_;
    bool started_{};
    std::vector<std::uint64_t> intervals_;
};
}
