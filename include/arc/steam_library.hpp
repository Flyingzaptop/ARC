#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arc {

struct SteamGame final {
    std::uint32_t app_id{};
    std::string name{};
    std::filesystem::path install_dir{};
    std::filesystem::path manifest_path{};
};

struct SteamLibraryScan final {
    std::filesystem::path steam_root{};
    std::vector<std::filesystem::path> library_roots{};
    std::vector<SteamGame> games{};
    std::vector<std::string> warnings{};
};

enum class GameProtectionKind : std::uint8_t {
    NoneDetected,
    EasyAntiCheat,
    BattlEye,
    OtherKnown,
};

struct GameProtection final {
    GameProtectionKind kind{GameProtectionKind::NoneDetected};
    std::string label{};
    std::filesystem::path evidence{};
    [[nodiscard]] bool protected_game() const noexcept { return kind != GameProtectionKind::NoneDetected; }
};

class SteamLibraryScanner final {
public:
    [[nodiscard]] static std::vector<std::filesystem::path> parse_library_folders(
        std::string_view vdf,
        const std::filesystem::path& steam_root);

    [[nodiscard]] static std::optional<SteamGame> parse_app_manifest(
        std::string_view acf,
        const std::filesystem::path& steamapps_dir,
        const std::filesystem::path& manifest_path = {});

    [[nodiscard]] static SteamLibraryScan scan(const std::filesystem::path& steam_root);
    [[nodiscard]] static GameProtection detect_protection(const std::filesystem::path& install_dir);
};

#ifdef _WIN32
[[nodiscard]] std::optional<std::filesystem::path> discover_steam_root_windows();
#endif

}  // namespace arc
