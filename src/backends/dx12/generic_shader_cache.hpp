#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <cstddef>
namespace arc::dx12::shader_cache {
std::string digest(std::span<const std::byte>);
std::string file_digest(const std::filesystem::path&);
bool restore(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary) noexcept;
bool store(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary) noexcept;
std::string restore_decline(const std::filesystem::path& root,const std::string& key) noexcept;
bool store_decline(const std::filesystem::path& root,const std::string& key,const std::string& reason) noexcept;
void trim(const std::filesystem::path& root,std::uintmax_t maximum_bytes=2ull*1024*1024*1024) noexcept;
}
