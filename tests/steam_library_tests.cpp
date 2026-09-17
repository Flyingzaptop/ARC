#include "arc/steam_library.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    const auto roots = SteamLibraryScanner::parse_library_folders(R"VDF(
"libraryfolders"
{
    "0"
    {
        "path"  "C:\\Program Files (x86)\\Steam"
    }
    "1"
    {
        "path"  "D:\\SteamLibrary"
    }
}
)VDF", "C:/Program Files (x86)/Steam");
    CHECK(roots.size() >= 2);

    const auto game = SteamLibraryScanner::parse_app_manifest(R"ACF(
"AppState"
{
    "appid"        "2406770"
    "Universe"     "1"
    "name"         "Bodycam"
    "StateFlags"   "4"
    "installdir"   "Bodycam"
}
)ACF", "D:/SteamLibrary/steamapps", "D:/SteamLibrary/steamapps/appmanifest_2406770.acf");
    CHECK(game.has_value());
    CHECK(game->app_id == 2406770);
    CHECK(game->name == "Bodycam");
    CHECK(game->install_dir.filename() == "Bodycam");

    CHECK(!SteamLibraryScanner::parse_app_manifest("\"appid\" \"wat\"", ".").has_value());
    CHECK(!SteamLibraryScanner::parse_app_manifest("\"appid\" \"1\" \"name\" \"x\"", ".").has_value());

    const auto temp = std::filesystem::temp_directory_path() / "arc-steam-library-test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp / "game" / "EasyAntiCheat", ec);
    {
        std::ofstream marker(temp / "game" / "EasyAntiCheat" / "EasyAntiCheat_EOS_Setup.exe");
        marker << "test";
    }
    const auto protection = SteamLibraryScanner::detect_protection(temp / "game");
    CHECK(protection.protected_game());
    CHECK(protection.kind == GameProtectionKind::EasyAntiCheat);
    std::filesystem::remove_all(temp, ec);

    return 0;
}
