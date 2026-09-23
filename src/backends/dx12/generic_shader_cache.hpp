#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <cstddef>
#include <cstdint>
namespace arc::dx12::shader_cache {
std::string digest(std::span<const std::byte>);
std::string file_digest(const std::filesystem::path&);
bool restore(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary) noexcept;
bool store(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary) noexcept;
std::string restore_decline(const std::filesystem::path& root,const std::string& key) noexcept;
bool store_decline(const std::filesystem::path& root,const std::string& key,const std::string& reason) noexcept;
bool stable_decline(std::string_view reason) noexcept;
struct CacheStats { std::uintmax_t bytes_high_water{}; std::uintmax_t last_scanned_bytes{}; std::uint64_t evicted_entries{}; };
CacheStats stats() noexcept;
void trim(const std::filesystem::path& root,std::uintmax_t maximum_bytes=2ull*1024*1024*1024) noexcept;
}
