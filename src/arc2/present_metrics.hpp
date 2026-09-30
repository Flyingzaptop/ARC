#pragma once
#include <cstdint>
namespace arc::arc2 { std::int64_t qpc_now(); void present_metric(std::uint64_t swapchain,std::int64_t begin,std::int64_t native_end,std::int32_t result,std::uint32_t flags); void save_present_metrics(const wchar_t* path); }
