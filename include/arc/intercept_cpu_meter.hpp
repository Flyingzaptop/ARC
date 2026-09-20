#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

namespace arc {
// Inclusive interceptor wall time, excluding only explicitly identified native
// application calls. Native calls made by ARC remain charged to ARC. Nested
// driver callbacks are measured separately only while the parent is excluded.
template<class Clock=std::chrono::steady_clock>
class BasicInterceptCpuMeter {
public:
    struct Snapshot {std::uint64_t own_ns{},excluded_native_ns{},calls{};};
    static void enable(bool value)noexcept{enabled_.store(value,std::memory_order_relaxed);}
    static bool enabled()noexcept{return enabled_.load(std::memory_order_relaxed);}
    static Snapshot snapshot()noexcept{return {own_.load(std::memory_order_relaxed),excluded_.load(std::memory_order_relaxed),calls_.load(std::memory_order_relaxed)};}
    class Native;
    class Scope {
        friend class Native;
        Scope* previous_{};
        bool linked_{},measured_{};
        unsigned native_depth_{};
        typename Clock::time_point started_{};
        std::uint64_t excluded_ns_{};
    public:
        explicit Scope(bool application_call)noexcept{
            if(!enabled()&&!current_)return;
            linked_=true;previous_=current_;current_=this;
            auto* owner=previous_;while(owner&&!owner->measured_)owner=owner->previous_;
            measured_=enabled()&&application_call&&(!owner||owner->native_depth_);
            if(measured_)started_=Clock::now();
        }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
        ~Scope(){
            if(measured_){const auto elapsed=nanoseconds(Clock::now()-started_);
                own_.fetch_add(elapsed>=excluded_ns_?elapsed-excluded_ns_:0,std::memory_order_relaxed);
                excluded_.fetch_add(excluded_ns_,std::memory_order_relaxed);calls_.fetch_add(1,std::memory_order_relaxed);}
            if(linked_)current_=previous_;
        }
    };
    class Native {
        Scope* owner_{};bool outer_{};typename Clock::time_point started_{};
    public:
        Native()noexcept{
            // Do not walk to an ancestor here: an internal ARC hook is an
            // intentional barrier preventing its native work being subtracted.
            if(current_&&current_->measured_){owner_=current_;outer_=owner_->native_depth_++==0;if(outer_)started_=Clock::now();}
        }
        Native(const Native&)=delete;
        Native& operator=(const Native&)=delete;
        ~Native(){if(owner_){if(outer_)owner_->excluded_ns_+=nanoseconds(Clock::now()-started_);--owner_->native_depth_;}}
    };
private:
    static std::uint64_t nanoseconds(typename Clock::duration value)noexcept{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());}
    inline static std::atomic<bool> enabled_{};
    inline static std::atomic<std::uint64_t> own_{},excluded_{},calls_{};
    inline static thread_local Scope* current_{};
};
using InterceptCpuMeter=BasicInterceptCpuMeter<>;
template<class F>decltype(auto) original_cpu_call(F&& call){InterceptCpuMeter::Native excluded;return std::forward<F>(call)();}
}
