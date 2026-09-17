#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace arc {

struct SteamGame final {
    std::uint32_t app_id{};
    std::string name{};
    std::filesystem::path install_dir{};
    std::filesystem::path manifest_path{};
};

[[nodiscard]] std::vector<std::filesystem::path> steam_library_paths(const std::filesystem::path& steam_root);
[[nodiscard]] std::optional<SteamGame> parse_steam_app_manifest(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& common_dir);
[[nodiscard]] std::vector<SteamGame> scan_installed_steam_games(const std::filesystem::path& steam_root);

}  // namespace arc
