#include "arc/steam_library.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>

namespace arc {
namespace {

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string unescape_vdf(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[i + 1];
            if (next == '\\' || next == '"') {
                output.push_back(next);
                ++i;
                continue;
            }
        }
        output.push_back(value[i]);
    }
    return output;
}

std::optional<std::string> quoted_value_after_key(const std::string& text, const std::string& key, std::size_t from = 0) {
    const std::string needle = '"' + key + '"';
    auto pos = text.find(needle, from);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    pos = text.find('"', pos);
    if (pos == std::string::npos) return std::nullopt;
    const auto end = text.find('"', pos + 1);
    if (end == std::string::npos) return std::nullopt;
    return unescape_vdf(text.substr(pos + 1, end - pos - 1));
}

std::vector<std::string> all_quoted_values_after_key(const std::string& text, const std::string& key) {
    std::vector<std::string> values;
    const std::string needle = '"' + key + '"';
    std::size_t search = 0;
    while (true) {
        auto pos = text.find(needle, search);
        if (pos == std::string::npos) break;
        pos += needle.size();
        const auto start = text.find('"', pos);
        if (start == std::string::npos) break;
        const auto end = text.find('"', start + 1);
        if (end == std::string::npos) break;
        values.push_back(unescape_vdf(text.substr(start + 1, end - start - 1)));
        search = end + 1;
    }
    return values;
}

std::optional<std::uint32_t> parse_u32(const std::string& value) {
    std::uint32_t result{};
    const auto* first = value.data();
    const auto* last = first + value.size();
    const auto parsed = std::from_chars(first, last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return std::nullopt;
    return result;
}

std::filesystem::path weak_normal(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : normalized;
}

}  // namespace

std::vector<std::filesystem::path> steam_library_paths(const std::filesystem::path& steam_root) {
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> unique;
    const auto add = [&](const std::filesystem::path& candidate) {
        const auto normalized = weak_normal(candidate);
        if (unique.insert(normalized).second) result.push_back(normalized);
    };

    add(steam_root);
    const auto text = read_text(steam_root / "steamapps" / "libraryfolders.vdf");
    if (!text) return result;
    for (const auto& value : all_quoted_values_after_key(*text, "path")) {
        if (!value.empty()) add(std::filesystem::u8path(value));
    }
    return result;
}

std::optional<SteamGame> parse_steam_app_manifest(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& common_dir) {
    const auto text = read_text(manifest_path);
    if (!text) return std::nullopt;
    const auto appid_text = quoted_value_after_key(*text, "appid");
    const auto name = quoted_value_after_key(*text, "name");
    const auto installdir = quoted_value_after_key(*text, "installdir");
    if (!appid_text || !name || !installdir) return std::nullopt;
    const auto app_id = parse_u32(*appid_text);
    if (!app_id || !*app_id || name->empty() || installdir->empty()) return std::nullopt;

    SteamGame game{};
    game.app_id = *app_id;
    game.name = *name;
    game.install_dir = weak_normal(common_dir / std::filesystem::u8path(*installdir));
    game.manifest_path = manifest_path;
    return game;
}

std::vector<SteamGame> scan_installed_steam_games(const std::filesystem::path& steam_root) {
    std::vector<SteamGame> games;
    std::set<std::uint32_t> seen;
    for (const auto& library : steam_library_paths(steam_root)) {
        const auto steamapps = library / "steamapps";
        std::error_code ec;
        if (!std::filesystem::is_directory(steamapps, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(steamapps, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const auto filename = entry.path().filename().string();
            if (!filename.starts_with("appmanifest_") || entry.path().extension() != ".acf") continue;
            auto game = parse_steam_app_manifest(entry.path(), steamapps / "common");
            if (!game || !seen.insert(game->app_id).second) continue;
            if (!std::filesystem::is_directory(game->install_dir, ec)) continue;
            games.push_back(std::move(*game));
        }
    }
    std::ranges::sort(games, [](const SteamGame& a, const SteamGame& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.app_id < b.app_id;
    });
    return games;
}

}  // namespace arc
