#ifdef _WIN32
#include "arc/steam_library.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int IDC_GAMES = 1001;
constexpr int IDC_SCAN = 1002;
constexpr int IDC_PLAY = 1003;
constexpr int IDC_ADVANCED = 1004;
constexpr int IDC_MODE = 1005;
constexpr int IDC_SECONDS = 1006;
constexpr int IDC_INSTALL_PM = 1007;
constexpr int IDC_STATUS = 1008;

HMENU control_id(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

std::vector<arc::SteamGame> g_games;
HWND g_list{}, g_mode{}, g_seconds{}, g_status{}, g_play{}, g_install_pm{};

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), chars);
    return out;
}

void status(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }

std::optional<std::filesystem::path> steam_root() {
    wchar_t value[2048]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS) {
        return std::filesystem::path(value);
    }
    const wchar_t* pf86 = _wgetenv(L"ProgramFiles(x86)");
    if (pf86) {
        auto fallback = std::filesystem::path(pf86) / L"Steam";
        std::error_code ec;
        if (std::filesystem::is_directory(fallback, ec)) return fallback;
    }
    return std::nullopt;
}

std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(std::wstring(path, path + n)).parent_path();
}

std::optional<std::filesystem::path> find_presentmon() {
    for (const wchar_t* name : {L"PresentMon.exe", L"PresentMon64.exe", L"PresentMon-2.5.1-x64.exe"}) {
        wchar_t out[MAX_PATH * 4]{};
        if (SearchPathW(nullptr, name, nullptr, static_cast<DWORD>(std::size(out)), out, nullptr)) return std::filesystem::path(out);
    }
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local) {
        for (const wchar_t* name : {L"PresentMon.exe", L"PresentMon-2.5.1-x64.exe"}) {
            auto p = std::filesystem::path(local) / L"Microsoft" / L"WinGet" / L"Links" / name;
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> guess_game_exe(const arc::SteamGame& game) {
    std::error_code ec;
    std::optional<std::filesystem::path> fallback;
    std::uintmax_t fallback_size{};
    int visited = 0;
    for (std::filesystem::recursive_directory_iterator it(game.install_dir, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (++visited > 20000) break;
        if (!it->is_regular_file(ec) || it->path().extension() != L".exe") continue;
        const auto filename = it->path().filename().wstring();
        std::wstring lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lower.ends_with(L"-win64-shipping.exe")) return it->path();
        const auto size = it->file_size(ec);
        if (!ec && size > fallback_size) { fallback = it->path(); fallback_size = size; }
    }
    return fallback;
}

std::wstring quote(const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; }

std::filesystem::path capture_dir(const arc::SteamGame& game) {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    auto root = local ? std::filesystem::path(local) / L"ARC" / L"captures" : exe_dir() / L"captures";
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, game.app_id);
    return root / stamp;
}

std::filesystem::path find_repo_script(const wchar_t* name) {
    for (auto base : {exe_dir(), exe_dir().parent_path(), exe_dir().parent_path().parent_path()}) {
        auto p = base / L"scripts" / name;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

bool start_process(const std::wstring& command) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}

void scan_games() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    const auto root = steam_root();
    if (!root) { status(L"Steam installation not found."); g_games.clear(); return; }
    g_games = arc::scan_installed_steam_games(*root);
    for (const auto& game : g_games) {
        const auto label = widen(game.name) + L"   [" + std::to_wstring(game.app_id) + L"]";
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    status(L"Found " + std::to_wstring(g_games.size()) + L" installed Steam games.");
}

void update_advanced(HWND hwnd) {
    const bool advanced = SendDlgItemMessageW(hwnd, IDC_ADVANCED, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ShowWindow(g_mode, advanced ? SW_SHOW : SW_HIDE);
    ShowWindow(g_seconds, advanced ? SW_SHOW : SW_HIDE);
    ShowWindow(g_install_pm, advanced ? SW_SHOW : SW_HIDE);
}

void play_selected(HWND hwnd) {
    const LRESULT selected = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<std::size_t>(selected) >= g_games.size()) { status(L"Select a game first."); return; }
    const auto& game = g_games[static_cast<std::size_t>(selected)];
    const auto uri = L"steam://rungameid/" + std::to_wstring(game.app_id);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        status(L"Steam launch failed."); return;
    }

    wchar_t sec_text[32]{}; GetWindowTextW(g_seconds, sec_text, static_cast<int>(std::size(sec_text)));
    const int seconds = (std::clamp)(_wtoi(sec_text), 15, 3600);
    const auto pm = find_presentmon();
    const auto monitor = exe_dir() / L"arc-game-monitor.exe";
    const auto session_script = find_repo_script(L"game-session.ps1");
    auto game_exe = guess_game_exe(game);
    auto out = capture_dir(game);
    std::error_code ec; std::filesystem::create_directories(out, ec);

    const int mode_index = static_cast<int>(SendMessageW(g_mode, CB_GETCURSEL, 0, 0));
    const std::wstring mode = mode_index == 0 ? L"baseline" : L"arc-observe";

    if (pm && game_exe && !session_script.empty()) {
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + quote(session_script) +
            L" -PresentMon " + quote(*pm) +
            L" -ProcessName \"" + game_exe->filename().wstring() + L"\"" +
            L" -CaptureDir " + quote(out) +
            L" -Seconds " + std::to_wstring(seconds) +
            L" -DelaySeconds 8 -Mode " + mode;
        std::error_code mec;
        if (std::filesystem::exists(monitor, mec)) cmd += L" -MonitorExe " + quote(monitor);
        start_process(cmd);
        status(L"Launched through Steam. Capture started: " + out.wstring());
    } else {
        std::wstring missing;
        if (!pm) missing += L" PresentMon";
        if (!game_exe) missing += L" game-exe";
        if (session_script.empty()) missing += L" session-script";
        status(L"Game launched. Telemetry unavailable; missing:" + missing);
    }
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"ARC Game Launcher", WS_CHILD|WS_VISIBLE, 18, 14, 260, 28, hwnd, nullptr, nullptr, nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr, WS_CHILD|WS_VISIBLE|LBS_NOTIFY|WS_VSCROLL, 18, 48, 560, 330, hwnd, control_id(IDC_GAMES), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Scan Steam Library", WS_CHILD|WS_VISIBLE, 18, 392, 150, 32, hwnd, control_id(IDC_SCAN), nullptr, nullptr);
        g_play = CreateWindowW(L"BUTTON", L"Play through ARC", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON, 180, 392, 150, 32, hwnd, control_id(IDC_PLAY), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Advanced", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, 350, 398, 100, 24, hwnd, control_id(IDC_ADVANCED), nullptr, nullptr);
        g_mode = CreateWindowW(WC_COMBOBOXW, nullptr, WS_CHILD|CBS_DROPDOWNLIST, 18, 438, 190, 200, hwnd, control_id(IDC_MODE), nullptr, nullptr);
        SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Baseline"));
        SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ARC External Observe"));
        SendMessageW(g_mode, CB_SETCURSEL, 1, 0);
        g_seconds = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"120", WS_CHILD|ES_NUMBER, 220, 438, 70, 26, hwnd, control_id(IDC_SECONDS), nullptr, nullptr);
        g_install_pm = CreateWindowW(L"BUTTON", L"Install PresentMon", WS_CHILD, 305, 435, 145, 30, hwnd, control_id(IDC_INSTALL_PM), nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Ready. External observe only; game resources are not mutated.", WS_CHILD|WS_VISIBLE, 18, 478, 560, 48, hwnd, control_id(IDC_STATUS), nullptr, nullptr);
        update_advanced(hwnd); scan_games(); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SCAN: scan_games(); return 0;
        case IDC_PLAY: play_selected(hwnd); return 0;
        case IDC_ADVANCED: update_advanced(hwnd); return 0;
        case IDC_INSTALL_PM:
            start_process(L"winget.exe install --id Intel.PresentMon.Console -e --accept-source-agreements --accept-package-agreements");
            status(L"PresentMon installation requested via winget."); return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    WNDCLASSW wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = instance; wc.lpszClassName = L"ARCLauncherWindow"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ARC Launcher", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 620, 575, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return static_cast<int>(msg.wParam);
}
#else
int main() { return 77; }
#endif
