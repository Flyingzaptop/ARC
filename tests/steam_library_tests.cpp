#include "arc/steam_library.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "CHECK failed: " #x " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "arc-steam-library-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "steamapps" / "common" / "Game A");
    fs::create_directories(root / "Library Two" / "steamapps" / "common" / "Game B");

    {
        std::ofstream out(root / "steamapps" / "libraryfolders.vdf");
        out << R"("libraryfolders"
{
  "0" { "path" ")" << root.string() << R"(" }
  "1" { "path" ")" << (root / "Library Two").string() << R"(" }
}
)";
    }
    {
        std::ofstream out(root / "steamapps" / "appmanifest_100.acf");
        out << R"("AppState" { "appid" "100" "name" "Game A" "installdir" "Game A" })";
    }
    {
        std::ofstream out(root / "Library Two" / "steamapps" / "appmanifest_200.acf");
        out << R"("AppState" { "appid" "200" "name" "Game B" "installdir" "Game B" })";
    }

    const auto libraries = arc::steam_library_paths(root);
    CHECK(libraries.size() == 2);
    const auto games = arc::scan_installed_steam_games(root);
    CHECK(games.size() == 2);
    CHECK(games[0].app_id == 100);
    CHECK(games[0].name == "Game A");
    CHECK(games[1].app_id == 200);
    CHECK(games[1].name == "Game B");

    fs::remove_all(root, ec);
    return 0;
}
