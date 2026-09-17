#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
constexpr int IDC_RUN = 3001;
constexpr int IDC_OPEN = 3002;
constexpr int IDC_STATUS = 3003;
constexpr int IDC_COPY = 3004;
constexpr UINT_PTR TIMER_RESULTS = 1;

HWND g_status{};
std::wstring g_last_text;

HMENU cid(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(path, path + n)).parent_path();
}

std::filesystem::path find_repo_root() {
    auto p = exe_dir();
    for (int i = 0; i < 7 && !p.empty(); ++i) {
        std::error_code ec;
        if (std::filesystem::exists(p / L".git", ec) || std::filesystem::exists(p / L"CMakeLists.txt", ec)) return p;
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

std::wstring quote(const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; }

void set_status(const std::wstring& text) {
    g_last_text = text;
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

std::wstring read_utf8_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) bytes.erase(0, 3);
    if (bytes.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return out;
}

std::filesystem::path local_results_root() {
    const auto repo = find_repo_root();
    return repo.empty() ? std::filesystem::path{} : repo / L"results" / L"stage7-local";
}

void refresh_results() {
    const auto root = local_results_root();
    if (root.empty()) return;
    const auto summary = root / L"last-summary.txt";
    std::error_code ec;
    if (!std::filesystem::exists(summary, ec)) return;
    const auto text = read_utf8_file(summary);
    if (!text.empty() && text != g_last_text) set_status(text);
}

bool launch_benchmark() {
    const auto repo = find_repo_root();
    if (repo.empty()) { set_status(L"Repository root not found."); return false; }
    const auto script = repo / L"scripts" / L"stage7-benchmark.ps1";
    std::error_code ec;
    if (!std::filesystem::exists(script, ec)) { set_status(L"stage7-benchmark.ps1 not found."); return false; }

    const auto root = repo / L"results" / L"stage7-local";
    std::filesystem::create_directories(root, ec);
    std::filesystem::remove(root / L"last-summary.txt", ec);
    std::filesystem::remove(root / L"last-result-url.txt", ec);

    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File " + quote(script) +
                        L" -RepoRoot " + quote(repo) + L" -Seconds 60 -ProbeFrames 18 -KeepOpen";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = args.c_str();
    sei.lpDirectory = repo.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        set_status(L"Could not start PowerShell benchmark process.");
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    set_status(L"RUNNING...\r\nMixed D3D12 graphics, native 1920x1080.\r\nBaseline -> measured micro-probes -> ARC adaptive.\r\nTemporal / DLSS / FSR / Frame Gen: OFF.");
    return true;
}

void copy_url(HWND hwnd) {
    const auto root = local_results_root();
    if (root.empty()) return;
    auto text = read_utf8_file(root / L"last-result-url.txt");
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    if (text.empty()) { MessageBoxW(hwnd, L"No published Results URL yet.", L"ARC Stage 7", MB_OK); return; }
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) { std::memcpy(p, text.c_str(), bytes); GlobalUnlock(mem); SetClipboardData(CF_UNICODETEXT, mem); }
        else GlobalFree(mem);
    }
    CloseClipboard();
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"ARC Stage 7 - Mixed Graphics Benchmark", WS_CHILD|WS_VISIBLE,
                      18, 14, 520, 26, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Real D3D12 geometry + raster + textures + lighting + shadows | Native 1920x1080", WS_CHILD|WS_VISIBLE,
                      18, 42, 690, 22, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Run 60s Mixed Benchmark", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                      18, 78, 220, 40, hwnd, cid(IDC_RUN), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Results Folder", WS_CHILD|WS_VISIBLE,
                      250, 78, 170, 40, hwnd, cid(IDC_OPEN), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Copy Results URL", WS_CHILD|WS_VISIBLE,
                      432, 78, 155, 40, hwnd, cid(IDC_COPY), nullptr, nullptr);
        g_status = CreateWindowW(L"EDIT", L"Idle. GPU smoke passed before this UI opens.\r\nTemporal / DLSS / FSR / Frame Gen: OFF.",
                                 WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,
                                 18, 135, 690, 250, hwnd, cid(IDC_STATUS), nullptr, nullptr);
        SetTimer(hwnd, TIMER_RESULTS, 1000, nullptr);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RUN) { launch_benchmark(); return 0; }
        if (LOWORD(wp) == IDC_OPEN) {
            const auto root = local_results_root();
            std::error_code ec; if (!root.empty()) std::filesystem::create_directories(root, ec);
            if (!root.empty()) ShellExecuteW(hwnd, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == IDC_COPY) { copy_url(hwnd); return 0; }
        break;
    case WM_TIMER:
        if (wp == TIMER_RESULTS) { refresh_results(); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_RESULTS);
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
    wc.lpszClassName = L"ARCStage7BenchmarkWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    SetLastError(ERROR_SUCCESS);
    const ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ARC Stage 7 Benchmark Center",
                              WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 750, 450,
                              nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return static_cast<int>(msg.wParam);
}

#else
int main() { return 77; }
#endif
