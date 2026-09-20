#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <array>

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
    struct Site {std::atomic<const char*> name{};std::atomic<std::uint64_t> own_ns{},calls{};};
    static unsigned register_site(const char* name)noexcept{const auto id=site_count_.fetch_add(1);if(id<sites_.size())sites_[id].name=name;return id;}
    static const auto& sites()noexcept{return sites_;}
    class Native;
    class Scope {
        friend class Native;
        Scope* previous_{};
        bool linked_{},measured_{};
        unsigned native_depth_{};
        typename Clock::time_point started_{};
        std::uint64_t excluded_ns_{};
        unsigned site_{UINT32_MAX};
    public:
        explicit Scope(bool application_call,unsigned site=UINT32_MAX)noexcept:site_(site){
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
                const auto own=elapsed>=excluded_ns_?elapsed-excluded_ns_:0;own_.fetch_add(own,std::memory_order_relaxed);
                excluded_.fetch_add(excluded_ns_,std::memory_order_relaxed);calls_.fetch_add(1,std::memory_order_relaxed);
                if(site_<sites_.size()){sites_[site_].own_ns.fetch_add(own,std::memory_order_relaxed);sites_[site_].calls.fetch_add(1,std::memory_order_relaxed);}}
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
    inline static std::array<Site,256> sites_;
    inline static std::atomic<unsigned> site_count_{};
};
using InterceptCpuMeter=BasicInterceptCpuMeter<>;
template<class F>decltype(auto) original_cpu_call(F&& call){InterceptCpuMeter::Native excluded;return std::forward<F>(call)();}
}
