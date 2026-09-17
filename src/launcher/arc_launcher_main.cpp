#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "arc/steam_library.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSearch = 1001;
constexpr int kList = 1002;
constexpr int kRefresh = 1003;
constexpr int kPlay = 1004;
constexpr int kReports = 1005;
constexpr int kAdvanced = 1006;
constexpr int kStatus = 1007;
constexpr int kAdvancedText = 1008;
constexpr UINT kAsyncStatus = WM_APP + 1;

HWND g_window{};
HWND g_search{};
HWND g_list{};
HWND g_status{};
HWND g_advanced_text{};
std::vector<arc::SteamGame> g_games;
std::vector<std::size_t> g_visible;
std::filesystem::path g_reports_root;
std::filesystem::path g_exe_dir;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring window_text(HWND control) {
    const auto length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const auto copied = GetWindowTextW(control, text.data(), length + 1);
    if (copied <= 0) return {};
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void status(std::wstring text) {
    SetWindowTextW(g_status, text.c_str());
}

void async_status(std::wstring text) {
    auto* copy = new std::wstring(std::move(text));
    if (!PostMessageW(g_window, kAsyncStatus, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

std::filesystem::path documents_arc_sessions() {
    PWSTR raw{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw))) {
        std::filesystem::path path(raw);
        CoTaskMemFree(raw);
        return path / L"ARC" / L"Sessions";
    }
    wchar_t user[MAX_PATH]{};
    GetEnvironmentVariableW(L"USERPROFILE", user, MAX_PATH);
    return std::filesystem::path(user) / L"Documents" / L"ARC" / L"Sessions";
}

std::wstring timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

void configure_list_columns() {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(L"Game"); column.cx = 260; ListView_InsertColumn(g_list, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"App ID"); column.cx = 90; ListView_InsertColumn(g_list, 1, &column);
    column.pszText = const_cast<LPWSTR>(L"Install path"); column.cx = 420; ListView_InsertColumn(g_list, 2, &column);
}

void refill_list() {
    ListView_DeleteAllItems(g_list);
    g_visible.clear();
    const auto filter = lowercase(window_text(g_search));
    for (std::size_t index = 0; index < g_games.size(); ++index) {
        const auto game_name = widen(g_games[index].name);
        if (!filter.empty() && lowercase(game_name).find(filter) == std::wstring::npos) continue;
        const auto row = static_cast<int>(g_visible.size());
        g_visible.push_back(index);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = const_cast<LPWSTR>(game_name.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(g_list, &item);
        const auto appid = std::to_wstring(g_games[index].app_id);
        ListView_SetItemText(g_list, row, 1, const_cast<LPWSTR>(appid.c_str()));
        const auto path = g_games[index].install_dir.wstring();
        ListView_SetItemText(g_list, row, 2, const_cast<LPWSTR>(path.c_str()));
    }
    status(L"Found " + std::to_wstring(g_visible.size()) + L" installed Steam games.");
}

void scan_steam() {
    const auto root = arc::discover_steam_root_windows();
    if (!root) {
        g_games.clear(); refill_list();
        status(L"Steam installation was not found.");
        return;
    }
    const auto scan = arc::SteamLibraryScanner::scan(*root);
    g_games = scan.games;
    refill_list();
}

std::optional<std::size_t> selected_game_index() {
    const auto row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (row < 0) return std::nullopt;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(g_list, &item)) return std::nullopt;
    const auto index = static_cast<std::size_t>(item.lParam);
    if (index >= g_games.size()) return std::nullopt;
    return index;
}

bool path_under(std::filesystem::path child, std::filesystem::path parent) {
    child = child.lexically_normal(); parent = parent.lexically_normal();
    auto a = child.begin(); auto b = parent.begin();
    for (; b != parent.end(); ++a, ++b) {
        if (a == child.end()) return false;
        if (_wcsicmp(a->c_str(), b->c_str()) != 0) return false;
    }
    return true;
}

bool excluded_process_name(std::wstring name) {
    name = lowercase(std::move(name));
    constexpr const wchar_t* blocked[] = {
        L"easyanticheat", L"beservice", L"battleye", L"crashreport", L"unrealcef", L"cefsubprocess", L"launcher"
    };
    for (const auto* token : blocked) if (name.find(token) != std::wstring::npos) return true;
    return false;
}

struct ProcessCandidate {
    DWORD pid{};
    std::filesystem::path exe{};
    bool shipping{};
};

std::optional<ProcessCandidate> find_game_process(const std::filesystem::path& install_dir) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<ProcessCandidate> fallback;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID <= 4 || excluded_process_name(entry.szExeFile)) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path_buffer[32768]{}; DWORD size = 32768;
            const bool ok = QueryFullProcessImageNameW(process, 0, path_buffer, &size) != FALSE;
            CloseHandle(process);
            if (!ok) continue;
            const std::filesystem::path exe(path_buffer);
            if (!path_under(exe, install_dir)) continue;
            const auto lowered = lowercase(exe.filename().wstring());
            ProcessCandidate candidate{entry.th32ProcessID, exe, lowered.find(L"shipping") != std::wstring::npos};
            if (candidate.shipping) { CloseHandle(snapshot); return candidate; }
            fallback = candidate;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return fallback;
}

bool launch_monitor(
    const arc::SteamGame& game,
    const arc::GameProtection& protection,
    DWORD pid,
    const std::filesystem::path& session) {
    const auto monitor = g_exe_dir / L"arc-game-monitor.exe";
    const auto presentmon = g_exe_dir / L"PresentMon.exe";
    if (!std::filesystem::exists(monitor)) return false;

    std::wstring command = L"\"" + monitor.wstring() + L"\" --pid " + std::to_wstring(pid) +
        L" --name \"" + widen(game.name) + L"\" --out \"" + session.wstring() + L"\"" +
        L" --mode \"" + std::wstring(protection.protected_game() ? L"external-observe-protected" : L"external-observe") + L"\"" +
        L" --protected " + std::wstring(protection.protected_game() ? L"1" : L"0");
    if (std::filesystem::exists(presentmon)) command += L" --presentmon \"" + presentmon.wstring() + L"\"";

    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

void launch_selected() {
    const auto index = selected_game_index();
    if (!index) { status(L"Select a game first."); return; }
    const auto game = g_games[*index];
    status(L"Checking game protection...");
    const auto protection = arc::SteamLibraryScanner::detect_protection(game.install_dir);

    std::error_code ec;
    g_reports_root = documents_arc_sessions();
    std::filesystem::create_directories(g_reports_root, ec);
    const auto session = g_reports_root / (timestamp() + L"-" + std::to_wstring(game.app_id));
    std::filesystem::create_directories(session, ec);

    std::ofstream meta(session / "launch.txt");
    meta << "appid=" << game.app_id << "\nname=" << game.name << "\ninstall=" << game.install_dir.string()
         << "\nprotection=" << protection.label << "\n";

    const auto uri = L"steam://rungameid/" + std::to_wstring(game.app_id);
    const auto launched = reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (launched <= 32) { status(L"Steam launch failed."); return; }

    const auto protection_text = protection.protected_game() ? widen(protection.label) : L"none detected";
    status(L"Steam started " + widen(game.name) + L". Waiting for the game process... Protection: " + protection_text);

    std::thread([game, protection, session] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        std::optional<ProcessCandidate> candidate;
        while (std::chrono::steady_clock::now() < deadline) {
            candidate = find_game_process(game.install_dir);
            if (candidate) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!candidate) {
            async_status(L"Game process was not found inside the Steam install directory. Session: " + session.wstring());
            return;
        }
        if (!launch_monitor(game, protection, candidate->pid, session)) {
            async_status(L"Game started, but ARC monitor could not start. Session: " + session.wstring());
            return;
        }
        async_status(L"ARC external capture attached to PID " + std::to_wstring(candidate->pid) + L". Play normally; report is saved after exit.");
    }).detach();
}

void update_selection_status() {
    const auto index = selected_game_index();
    if (!index) return;
    const auto& game = g_games[*index];
    status(L"Selected: " + widen(game.name) + L". Protection scan runs when you press Play + Capture.");
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 18, 420, 28, hwnd, reinterpret_cast<HMENU>(kSearch), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Refresh Steam", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            450, 18, 120, 28, hwnd, reinterpret_cast<HMENU>(kRefresh), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Play + Capture", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            580, 18, 130, 28, hwnd, reinterpret_cast<HMENU>(kPlay), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Reports", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            720, 18, 120, 28, hwnd, reinterpret_cast<HMENU>(kReports), nullptr, nullptr);

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            16, 58, 824, 420, hwnd, reinterpret_cast<HMENU>(kList), nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        configure_list_columns();

        CreateWindowW(L"BUTTON", L"Advanced", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 490, 100, 24, hwnd, reinterpret_cast<HMENU>(kAdvanced), nullptr, nullptr);
        g_advanced_text = CreateWindowW(L"STATIC",
            L"Safe mode: Steam launch + external ETW/DXGI telemetry. Controlled in-process mode is enabled only for an explicit ARC adapter; protected games stay external-only.",
            WS_CHILD, 125, 492, 715, 42, hwnd, reinterpret_cast<HMENU>(kAdvancedText), nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Scanning Steam...", WS_CHILD | WS_VISIBLE,
            16, 542, 824, 38, hwnd, reinterpret_cast<HMENU>(kStatus), nullptr, nullptr);
        scan_steam();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kRefresh && HIWORD(wparam) == BN_CLICKED) scan_steam();
        else if (LOWORD(wparam) == kPlay && HIWORD(wparam) == BN_CLICKED) launch_selected();
        else if (LOWORD(wparam) == kReports && HIWORD(wparam) == BN_CLICKED) {
            if (g_reports_root.empty()) g_reports_root = documents_arc_sessions();
            std::error_code ec; std::filesystem::create_directories(g_reports_root, ec);
            ShellExecuteW(hwnd, L"open", g_reports_root.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (LOWORD(wparam) == kAdvanced && HIWORD(wparam) == BN_CLICKED) {
            const auto checked = SendMessageW(reinterpret_cast<HWND>(lparam), BM_GETCHECK, 0, 0) == BST_CHECKED;
            ShowWindow(g_advanced_text, checked ? SW_SHOW : SW_HIDE);
        } else if (LOWORD(wparam) == kSearch && HIWORD(wparam) == EN_CHANGE) refill_list();
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header->idFrom == kList && header->code == LVN_ITEMCHANGED) update_selection_status();
        return 0;
    }
    case kAsyncStatus: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        if (text) status(*text);
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        if (g_list) MoveWindow(g_list, 16, 58, std::max(100, width - 32), std::max(120, height - 180), TRUE);
        if (g_status) MoveWindow(g_status, 16, std::max(100, height - 48), std::max(100, width - 32), 38, TRUE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    wchar_t exe_path[32768]{};
    GetModuleFileNameW(nullptr, exe_path, 32768);
    g_exe_dir = std::filesystem::path(exe_path).parent_path();
    g_reports_root = documents_arc_sessions();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"ARCSteamLauncher";
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&cls)) return 1;

    g_window = CreateWindowExW(0, cls.lpszClassName, L"ARC — Steam Launcher / External Observe",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 880, 640, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 2;
    ShowWindow(g_window, show);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

#else
int main() { return 77; }
#endif
