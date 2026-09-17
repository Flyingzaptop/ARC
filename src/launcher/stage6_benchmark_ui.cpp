#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <sstream>
#include <string>

namespace {
constexpr int IDC_RUN = 2001;
constexpr int IDC_OPEN = 2002;
constexpr int IDC_STATUS = 2003;

HWND g_status{};

HMENU cid(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(path, path + n)).parent_path();
}

std::filesystem::path find_repo_root() {
    auto p = exe_dir();
    for (int i = 0; i < 6 && !p.empty(); ++i) {
        std::error_code ec;
        if (std::filesystem::exists(p / L".git", ec) || std::filesystem::exists(p / L"CMakeLists.txt", ec)) return p;
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

std::wstring quote(const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; }

void show_last_error(const wchar_t* what) {
    const DWORD code = GetLastError();
    wchar_t* system_message = nullptr;
    const DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);

    std::wstringstream ss;
    ss << what << L" failed (Win32 error " << code << L")";
    if (chars != 0 && system_message) ss << L"\n\n" << system_message;
    MessageBoxW(nullptr, ss.str().c_str(), L"ARC Stage 6 Benchmark", MB_OK | MB_ICONERROR);
    if (system_message) LocalFree(system_message);
}

void set_status(const wchar_t* text) {
    if (g_status) SetWindowTextW(g_status, text);
}

bool launch_benchmark() {
    const auto repo = find_repo_root();
    if (repo.empty()) { set_status(L"Repository root not found."); return false; }
    const auto script = repo / L"scripts" / L"stage6-benchmark.ps1";
    std::error_code ec;
    if (!std::filesystem::exists(script, ec)) { set_status(L"stage6-benchmark.ps1 not found."); return false; }

    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File " + quote(script) +
                        L" -RepoRoot " + quote(repo) + L" -Seconds 60";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = args.c_str();
    sei.lpDirectory = repo.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        show_last_error(L"ShellExecuteExW");
        set_status(L"Could not start benchmark process.");
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    set_status(L"Benchmark running (~60 s). Result will publish automatically.");
    return true;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"ARC Stage 6 - Adaptive Quality Core", WS_CHILD|WS_VISIBLE,
                      18, 16, 440, 28, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Native 100% rendering. Temporal / DLSS / FSR / Frame Gen: OFF", WS_CHILD|WS_VISIBLE,
                      18, 48, 520, 24, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Run 60s Benchmark", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                      18, 88, 190, 38, hwnd, cid(IDC_RUN), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Results Folder", WS_CHILD|WS_VISIBLE,
                      220, 88, 170, 38, hwnd, cid(IDC_OPEN), nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Idle. Close games before benchmarking.", WS_CHILD|WS_VISIBLE,
                                 18, 145, 520, 60, hwnd, cid(IDC_STATUS), nullptr, nullptr);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RUN) { launch_benchmark(); return 0; }
        if (LOWORD(wp) == IDC_OPEN) {
            const auto repo = find_repo_root();
            const auto results = repo.empty() ? std::filesystem::path{} : repo / L"results" / L"stage6-local";
            std::error_code ec;
            if (!results.empty()) std::filesystem::create_directories(results, ec);
            if (!results.empty()) ShellExecuteW(hwnd, L"open", results.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ARCStage6BenchmarkWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    SetLastError(ERROR_SUCCESS);
    const ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_last_error(L"RegisterClassW");
        return 1;
    }

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"ARC Stage 6 Benchmark",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        580,
        260,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd) {
        show_last_error(L"CreateWindowW");
        return 2;
    }

    ShowWindow(hwnd, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
#else
int main() { return 77; }
#endif
