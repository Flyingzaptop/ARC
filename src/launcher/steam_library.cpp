#include "arc/steam_library.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace arc {
namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::string> quoted_value_after_key(std::string_view text, std::string_view wanted_key) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto key_open = text.find('"', cursor);
        if (key_open == std::string_view::npos) break;
        const auto key_close = text.find('"', key_open + 1);
        if (key_close == std::string_view::npos) break;
        const auto key = text.substr(key_open + 1, key_close - key_open - 1);
        cursor = key_close + 1;
        if (key != wanted_key) continue;

        const auto value_open = text.find('"', cursor);
        if (value_open == std::string_view::npos) return std::nullopt;
        const auto value_close = text.find('"', value_open + 1);
        if (value_close == std::string_view::npos) return std::nullopt;
        return std::string(text.substr(value_open + 1, value_close - value_open - 1));
    }
    return std::nullopt;
}

std::string unescape_vdf_path(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
            output.push_back('\\');
            ++i;
        } else {
            output.push_back(value[i]);
        }
    }
    return output;
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}

bool filename_contains(const std::filesystem::path& path, std::string_view needle) {
    auto name = lowercase_ascii(path.filename().string());
    return name.find(needle) != std::string::npos;
}

}  // namespace

std::vector<std::filesystem::path> SteamLibraryScanner::parse_library_folders(
    std::string_view vdf,
    const std::filesystem::path& steam_root) {
    std::vector<std::filesystem::path> roots;
    roots.push_back(normalized(steam_root));

    std::size_t cursor = 0;
    while (cursor < vdf.size()) {
        const auto key = vdf.find("\"path\"", cursor);
        if (key == std::string_view::npos) break;
        const auto value_open = vdf.find('"', key + 6);
        if (value_open == std::string_view::npos) break;
        const auto value_close = vdf.find('"', value_open + 1);
        if (value_close == std::string_view::npos) break;
        auto value = unescape_vdf_path(std::string(vdf.substr(value_open + 1, value_close - value_open - 1)));
        if (!value.empty()) roots.push_back(normalized(std::filesystem::path(value)));
        cursor = value_close + 1;
    }

    std::set<std::filesystem::path> seen;
    std::vector<std::filesystem::path> unique;
    for (auto& root : roots) {
        const auto key = root.lexically_normal();
        if (seen.insert(key).second) unique.push_back(std::move(root));
    }
    return unique;
}

std::optional<SteamGame> SteamLibraryScanner::parse_app_manifest(
    std::string_view acf,
    const std::filesystem::path& steamapps_dir,
    const std::filesystem::path& manifest_path) {
    const auto appid_text = quoted_value_after_key(acf, "appid");
    const auto name = quoted_value_after_key(acf, "name");
    const auto install_dir_name = quoted_value_after_key(acf, "installdir");
    if (!appid_text || !name || !install_dir_name || appid_text->empty() || install_dir_name->empty()) {
        return std::nullopt;
    }

    std::uint64_t appid64{};
    try {
        std::size_t consumed{};
        appid64 = std::stoull(*appid_text, &consumed, 10);
        if (consumed != appid_text->size() || appid64 == 0 || appid64 > 0xffffffffULL) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }

    SteamGame game{};
    game.app_id = static_cast<std::uint32_t>(appid64);
    game.name = *name;
    game.install_dir = normalized(steamapps_dir / "common" / std::filesystem::path(*install_dir_name));
    game.manifest_path = manifest_path;
    return game;
}

SteamLibraryScan SteamLibraryScanner::scan(const std::filesystem::path& steam_root) {
    SteamLibraryScan result{};
    result.steam_root = normalized(steam_root);
    if (result.steam_root.empty()) {
        result.warnings.emplace_back("Steam root is empty");
        return result;
    }

    const auto library_file = result.steam_root / "steamapps" / "libraryfolders.vdf";
    const auto library_text = read_text(library_file);
    result.library_roots = parse_library_folders(library_text, result.steam_root);
    if (library_text.empty()) {
        result.warnings.emplace_back("libraryfolders.vdf could not be read; scanning the primary Steam library only");
    }

    std::set<std::uint32_t> seen_apps;
    for (const auto& root : result.library_roots) {
        const auto steamapps = root / "steamapps";
        std::error_code ec;
        if (!std::filesystem::is_directory(steamapps, ec)) continue;

        for (std::filesystem::directory_iterator it(steamapps, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto filename = it->path().filename().string();
            if (!filename.starts_with("appmanifest_") || !filename.ends_with(".acf")) continue;
            const auto text = read_text(it->path());
            auto game = parse_app_manifest(text, steamapps, it->path());
            if (!game || !std::filesystem::is_directory(game->install_dir, ec)) continue;
            if (seen_apps.insert(game->app_id).second) result.games.push_back(std::move(*game));
        }
    }

    std::ranges::sort(result.games, [](const SteamGame& a, const SteamGame& b) {
        const auto an = lowercase_ascii(a.name);
        const auto bn = lowercase_ascii(b.name);
        if (an != bn) return an < bn;
        return a.app_id < b.app_id;
    });
    return result;
}

GameProtection SteamLibraryScanner::detect_protection(const std::filesystem::path& install_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(install_dir, ec)) return {};

    std::size_t inspected{};
    constexpr std::size_t kMaxEntries = 12000;
    std::filesystem::recursive_directory_iterator it(
        install_dir,
        std::filesystem::directory_options::skip_permission_denied,
        ec), end;
    while (!ec && it != end && inspected++ < kMaxEntries) {
        const auto path = it->path();
        const auto depth = it.depth();
        if (depth > 6 && it->is_directory(ec)) it.disable_recursion_pending();

        if (filename_contains(path, "easyanticheat")) {
            return {GameProtectionKind::EasyAntiCheat, "Easy Anti-Cheat", path};
        }
        if (filename_contains(path, "battleye") || filename_contains(path, "beservice")) {
            return {GameProtectionKind::BattlEye, "BattlEye", path};
        }
        if (filename_contains(path, "anticheatexpert") || filename_contains(path, "ace-base") ||
            filename_contains(path, "xigncode") || filename_contains(path, "vgk")) {
            return {GameProtectionKind::OtherKnown, "Known anti-cheat", path};
        }
        it.increment(ec);
    }
    return {};
}

#ifdef _WIN32
std::optional<std::filesystem::path> discover_steam_root_windows() {
    wchar_t buffer[4096]{};
    DWORD bytes = sizeof(buffer);
    DWORD type{};
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam",
            L"SteamPath",
            RRF_RT_REG_SZ,
            &type,
            buffer,
            &bytes) == ERROR_SUCCESS) {
        return normalized(std::filesystem::path(buffer));
    }

    wchar_t program_files[MAX_PATH]{};
    const auto count = GetEnvironmentVariableW(L"ProgramFiles(x86)", program_files, MAX_PATH);
    if (count > 0 && count < MAX_PATH) {
        const auto fallback = std::filesystem::path(program_files) / L"Steam";
        std::error_code ec;
        if (std::filesystem::is_directory(fallback, ec)) return normalized(fallback);
    }
    return std::nullopt;
}
#endif

}  // namespace arc
