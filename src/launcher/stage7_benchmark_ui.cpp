#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
constexpr int IDC_RUN = 3001;
constexpr int IDC_OPEN = 3002;
constexpr int IDC_COPY = 3003;
constexpr int IDC_STATUS = 3004;
constexpr UINT_PTR TIMER_RESULT = 4001;

HWND g_status{};
HWND g_run{};
std::wstring g_last_url;
FILETIME g_last_write{};

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

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::map<std::string,std::string> read_kv(const std::filesystem::path& p) {
    std::map<std::string,std::string> kv;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f,line)) {
        const auto eq=line.find('=');
        if (eq==std::string::npos) continue;
        kv[line.substr(0,eq)] = line.substr(eq+1);
    }
    return kv;
}

bool newer_result(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_last_write) <= 0) return false;
    g_last_write = data.ftLastWriteTime;
    return true;
}

void set_status(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void update_from_result() {
    const auto repo=find_repo_root();
    if (repo.empty()) return;
    const auto p=repo/L"results"/L"stage7-local"/L"last-result.txt";
    if (!std::filesystem::exists(p) || !newer_result(p)) return;
    const auto kv=read_kv(p);
    auto get=[&](const char* k)->std::string { const auto it=kv.find(k); return it==kv.end()?std::string{}:it->second; };
    g_last_url=widen(get("url"));
    std::wstringstream s;
    s << L"Verdict: " << widen(get("verdict")) << L"\r\n"
      << L"P50: " << widen(get("baseline_p50_ms")) << L" -> " << widen(get("adaptive_p50_ms"))
      << L" ms  (delta " << widen(get("p50_delta_ms")) << L")\r\n"
      << L"P99 delta: " << widen(get("p99_delta_ms")) << L" ms   Actions: " << widen(get("selected_actions")) << L"\r\n"
      << L"Pass delta ms: shadow " << widen(get("shadow_delta_ms"))
      << L", geometry " << widen(get("geometry_delta_ms"))
      << L", raster " << widen(get("raster_delta_ms"))
      << L", texture " << widen(get("texture_delta_ms"))
      << L", lighting " << widen(get("lighting_delta_ms")) << L"\r\n"
      << L"Results: " << (g_last_url.empty()?L"not published":g_last_url);
    set_status(s.str());
    if (g_run) EnableWindow(g_run, TRUE);
}

bool launch_benchmark() {
    const auto repo=find_repo_root();
    if (repo.empty()) { set_status(L"Repository root not found."); return false; }
    const auto script=repo/L"scripts"/L"stage7-benchmark.ps1";
    if (!std::filesystem::exists(script)) { set_status(L"stage7-benchmark.ps1 not found."); return false; }
    const auto last=repo/L"results"/L"stage7-local"/L"last-result.txt";
    std::error_code ec; std::filesystem::remove(last,ec); g_last_write={}; g_last_url.clear();

    std::wstring args=L"-NoProfile -ExecutionPolicy Bypass -File "+quote(script)+
                      L" -RepoRoot "+quote(repo)+L" -Seconds 60 -KeepOpen";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask=SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb=L"open";
    sei.lpFile=L"powershell.exe";
    sei.lpParameters=args.c_str();
    sei.lpDirectory=repo.c_str();
    sei.nShow=SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) { set_status(L"Could not start benchmark PowerShell."); return false; }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    if (g_run) EnableWindow(g_run,FALSE);
    set_status(L"Running 60 s mixed graphics benchmark...\r\nPowerShell will stay open after completion.\r\nResults will also appear here automatically.");
    return true;
}

void copy_url(HWND hwnd) {
    if (g_last_url.empty()) { MessageBoxW(hwnd,L"No published Results URL yet.",L"ARC Stage 7",MB_OK|MB_ICONINFORMATION); return; }
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const SIZE_T bytes=(g_last_url.size()+1)*sizeof(wchar_t);
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if (h) {
        void* p=GlobalLock(h);
        if (p) {
            std::memcpy(p,g_last_url.c_str(),bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT,h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK wndproc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        CreateWindowW(L"STATIC",L"ARC Stage 7 - Mixed Graphics Benchmark",WS_CHILD|WS_VISIBLE,
                      18,14,600,26,hwnd,nullptr,nullptr,nullptr);
        CreateWindowW(L"STATIC",L"Native 100% rendering. Temporal / DLSS / FSR / Frame Gen: OFF",WS_CHILD|WS_VISIBLE,
                      18,42,650,22,hwnd,nullptr,nullptr,nullptr);
        CreateWindowW(L"STATIC",L"Real D3D12 passes: shadow / geometry / raster / texture-bandwidth / lighting",WS_CHILD|WS_VISIBLE,
                      18,66,700,22,hwnd,nullptr,nullptr,nullptr);
        g_run=CreateWindowW(L"BUTTON",L"Run 60s Mixed Benchmark",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                           18,102,220,40,hwnd,cid(IDC_RUN),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"Open Results Folder",WS_CHILD|WS_VISIBLE,
                      250,102,180,40,hwnd,cid(IDC_OPEN),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"Copy Results URL",WS_CHILD|WS_VISIBLE,
                      442,102,170,40,hwnd,cid(IDC_COPY),nullptr,nullptr);
        g_status=CreateWindowW(L"EDIT",L"Idle. Close games and GPU-heavy applications before benchmarking.",
                              WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                              18,158,700,180,hwnd,cid(IDC_STATUS),nullptr,nullptr);
        SetTimer(hwnd,TIMER_RESULT,1000,nullptr);
        return 0;
    case WM_TIMER:
        if (wp==TIMER_RESULT) { update_from_result(); return 0; }
        break;
    case WM_COMMAND:
        if (LOWORD(wp)==IDC_RUN) { launch_benchmark(); return 0; }
        if (LOWORD(wp)==IDC_OPEN) {
            const auto repo=find_repo_root();
            if (!repo.empty()) {
                const auto p=repo/L"results"/L"stage7-local";
                std::error_code ec; std::filesystem::create_directories(p,ec);
                ShellExecuteW(hwnd,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            }
            return 0;
        }
        if (LOWORD(wp)==IDC_COPY) { copy_url(hwnd); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(hwnd,TIMER_RESULT);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    WNDCLASSW wc{};
    wc.lpfnWndProc=wndproc;
    wc.hInstance=instance;
    wc.lpszClassName=L"ARCStage7MixedBenchmarkWindow";
    wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    SetLastError(ERROR_SUCCESS);
    const ATOM atom=RegisterClassW(&wc);
    if (!atom && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return 1;
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"ARC Stage 7 Mixed Graphics Benchmark",
                            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                            CW_USEDEFAULT,CW_USEDEFAULT,760,400,nullptr,nullptr,instance,nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd,show==0?SW_SHOWNORMAL:show);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return static_cast<int>(msg.wParam);
}
#else
int main(){return 77;}
#endif
