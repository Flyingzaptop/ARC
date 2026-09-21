#ifdef _WIN32
#include "arc/steam_library.hpp"
#include "arc_overlay.hpp"
#include "json.hpp"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cmath>

namespace {
using Json=nlohmann::json;
enum { Games=1001,Scan,Choose,Launch,Attach,Pid,Target,ApplyTarget,Stop,Arguments,SteamOptions,Overlay };
HWND list{},path_label{},arguments{},pid_edit{},target_edit{},state_label{},fps_label{},detail_label{};
std::vector<arc::SteamGame> games;
std::filesystem::path selected,session,metrics;
std::filesystem::path launch_scope;
bool helper_failed{},starting_game{};
DWORD diagnostics_pid{};bool diagnostics_requested{};std::wstring control_error;
ULONGLONG started_at{};
DWORD target_pid{};HANDLE helper{};
HFONT font{};
std::wstring wide(const std::string& value){if(value.empty())return {};const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);std::wstring result(size,0);MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),result.data(),size);return result;}
std::string utf8(const std::filesystem::path& path){const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
std::wstring quote(const std::wstring& value){std::wstring result=L"\"";unsigned slashes=0;for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}result.append(slashes*2,L'\\');return result+L'"';}
std::filesystem::path directory(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return std::filesystem::path(path).parent_path();}
std::filesystem::path storage(){const auto* local=_wgetenv(L"LOCALAPPDATA");return local?std::filesystem::path(local)/L"ARC":directory()/L"data";}
std::wstring text(HWND control){const auto size=GetWindowTextLengthW(control);std::wstring value(size+1,0);GetWindowTextW(control,value.data(),size+1);value.resize(size);return value;}
void status(const std::wstring& message){SetWindowTextW(state_label,message.c_str());}
HWND control(HWND parent,const wchar_t* type,const wchar_t* caption,DWORD style,int x,int y,int w,int h,int id=0){
    const auto child=CreateWindowExW(type==std::wstring(L"EDIT")||type==std::wstring(L"LISTBOX")?WS_EX_CLIENTEDGE:0,type,caption,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return child;
}
void scan(){
    wchar_t value[32768]{};DWORD bytes=sizeof(value);games.clear();SendMessageW(list,LB_RESETCONTENT,0,0);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS)games=arc::scan_installed_steam_games(value);
    for(const auto& game:games){const auto name=wide(game.name);SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
    status(games.empty()?L"Выберите EXE приложения. Библиотека Steam не найдена.":L"Выберите игру или укажите EXE приложения.");
}
std::filesystem::path game_executable(const arc::SteamGame& game){
    std::error_code error;std::filesystem::path best;std::uintmax_t size=0;unsigned visited=0;
    for(std::filesystem::recursive_directory_iterator it(game.install_dir,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end&&!error;it.increment(error)){
        if(++visited>20000)break;if(!it->is_regular_file(error)||it->path().extension()!=L".exe")continue;
        auto name=it->path().filename().wstring();std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towlower(c);});
        if(name.ends_with(L"-win64-shipping.exe"))return it->path();const auto bytes=it->file_size(error);if(!error&&bytes>size){size=bytes;best=it->path();}
    }return best;
}
double target(){const auto value=text(target_edit);std::size_t used{};const auto fps=std::stod(value,&used);if(used!=value.size()||!std::isfinite(fps)||fps<=0||fps>1000)throw std::runtime_error("Target FPS must be between 1 and 1000");return fps;}
std::filesystem::path configuration(bool launch){
    const auto root=directory();const auto runtime=root/L"runtime";
    const std::pair<const char*,std::filesystem::path> files[]{{"worker",root/L"arc-shader-tool.exe"},{"compiler",runtime/L"dxc/dxcompiler.dll"},{"python",runtime/L"python/python.exe"},{"critic",root/L"scripts/optimizer-live-quality.py"}};
    Json config{{"target_fps",target()},{"maximize_fps",true},{"diagnostics_overlay",arc::overlay::enabled()},{"maximum_seconds",0}};
    for(const auto& [key,path]:files){if(!std::filesystem::is_regular_file(path))throw std::runtime_error("Incomplete ARC package: "+utf8(path));config[key]=utf8(path);}
    if(!std::filesystem::is_regular_file(root/L"arc-dx12-probe.dll")||!std::filesystem::is_regular_file(root/L"arc-dx12-probe-launch.exe"))throw std::runtime_error("ARC runtime files are missing");
    SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[96]{};swprintf_s(stamp,L"%04u%02u%02u-%02u%02u%02u-%llu",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetTickCount64());
    session=storage()/L"sessions"/stamp;std::filesystem::create_directories(session);metrics=session/L"arc.json";
    config["output"]=utf8(session/L"automatic");config["cache"]=utf8(storage()/L"shader-cache");
    if(launch){config["launch_scope"]=utf8(launch_scope.empty()?selected.parent_path():launch_scope);config["session_root"]=utf8(session);config["launch_helper"]=utf8(root/L"arc-dx12-probe-launch.exe");}
    const auto file=session/L"config.json";std::ofstream out(file);out<<config.dump(2);out.close();if(!out)throw std::runtime_error("Cannot save ARC configuration");return file;
}
bool busy(){if(!helper)return false;if(WaitForSingleObject(helper,0)==WAIT_TIMEOUT)return true;CloseHandle(helper);helper=nullptr;return false;}
void command(const std::wstring& parameters){
    if(busy())throw std::runtime_error("The previous ARC request is still running");helper_failed=false;control_error.clear();
    if(session.empty()){session=storage()/L"sessions"/L"control";std::filesystem::create_directories(session);}
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};const auto log=session/L"launcher.log";
    HANDLE output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE){if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);throw std::runtime_error("Cannot open launcher log");}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;startup.hStdOutput=startup.hStdError=output;startup.hStdInput=input;
    PROCESS_INFORMATION process{};const auto exe=directory()/L"arc-dx12-probe-launch.exe";auto line=quote(exe.wstring())+L" "+parameters;
    const bool ok=CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,directory().c_str(),&startup,&process)!=FALSE;
    CloseHandle(output);CloseHandle(input);if(!ok)throw std::runtime_error("Cannot start ARC helper");CloseHandle(process.hThread);helper=process.hProcess;
}
void launch(bool attach){
    if(busy())throw std::runtime_error("ARC is still starting");if(target_pid)throw std::runtime_error("Stop the current ARC session first");
    DWORD pid=0;if(attach){const auto value=text(pid_edit);std::size_t used{};const auto number=std::stoul(value,&used);if(!number||number>MAXDWORD||used!=value.size())throw std::runtime_error("Enter a valid process ID");pid=DWORD(number);}
    else if(selected.empty()||!std::filesystem::is_regular_file(selected))throw std::runtime_error("Select an application EXE");
    const auto config=configuration(!attach);const auto dll=directory()/L"arc-dx12-probe.dll";
    std::wstring args=attach?L"--attach-auto "+std::to_wstring(pid):L"--launch-auto "+quote(selected.wstring());
    args+=L" "+quote(dll.wstring())+L" "+quote(metrics.wstring())+L" "+quote(config.wstring());if(!attach&&!text(arguments).empty())args+=L" "+text(arguments);
    command(args);starting_game=!attach;started_at=GetTickCount64();status(L"Подключение ARC…");SetWindowTextW(detail_label,session.c_str());
}
void steam_options(HWND owner){
    const auto value=quote((directory()/L"arc-launcher.exe").wstring())+L" --steam-launch "+std::to_wstring(target())+L" %command%";
    if(!OpenClipboard(owner))throw std::runtime_error("Cannot open clipboard");
    const auto bytes=(value.size()+1)*sizeof(wchar_t);HANDLE memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!memory){CloseClipboard();throw std::runtime_error("Clipboard allocation failed");}
    void* data=GlobalLock(memory);if(!data){GlobalFree(memory);CloseClipboard();throw std::runtime_error("Clipboard lock failed");}
    memcpy(data,value.c_str(),bytes);GlobalUnlock(memory);
    const bool ok=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory);if(!ok)GlobalFree(memory);CloseClipboard();
    if(!ok)throw std::runtime_error("Clipboard write failed");
    status(L"Строка скопирована. Steam → Свойства игры → Параметры запуска. Вставьте её и запускайте игру из Steam.");
}
void steam_launch_arguments(){
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)throw std::runtime_error("Cannot parse launch command");
    struct Free {LPWSTR* value;~Free(){LocalFree(value);}} free{args};
    if(count<2)return;
    if(std::wstring(args[1])==L"--monitor"&&count==3){metrics=std::filesystem::absolute(args[2]);if(!std::filesystem::is_regular_file(metrics))throw std::runtime_error("Existing metrics file required");session=metrics.parent_path();arc::overlay::toggle();SetWindowTextW(detail_label,session.c_str());return;}
    if(std::wstring(args[1])!=L"--steam-launch"||count<4)throw std::runtime_error("Expected --steam-launch <target FPS> <game EXE> [game arguments]");
    SetWindowTextW(target_edit,args[2]);target();
    selected=std::filesystem::absolute(args[3]);launch_scope=selected.parent_path();
    for(const auto& game:games){std::error_code error;const auto relative=std::filesystem::relative(selected,game.install_dir,error);if(!error&&!relative.empty()&&!relative.is_absolute()&&*relative.begin()!=L".."){launch_scope=game.install_dir;break;}}
    std::wstring parameters;for(int i=4;i<count;++i){if(!parameters.empty())parameters+=L' ';parameters+=quote(args[i]);}
    SetWindowTextW(path_label,selected.c_str());SetWindowTextW(arguments,parameters.c_str());
    // Preserve Steam's environment and the exact executable/arguments supplied
    // by %command%; do not substitute a guessed Shipping executable.
    launch(false);
}
void stop(){if(!target_pid)return;command(L"--stop-auto "+std::to_wstring(target_pid)+L" "+quote((directory()/L"arc-dx12-probe.dll").wstring()));status(L"Восстановление исходного рендера…");}
void poll(){
    if(helper&&WaitForSingleObject(helper,0)==WAIT_OBJECT_0){DWORD result{};GetExitCodeProcess(helper,&result);CloseHandle(helper);helper=nullptr;if(result){helper_failed=true;std::ifstream f(session/L"launcher.log");const std::string error{std::istreambuf_iterator<char>(f),{}};control_error=L"Ошибка команды: "+wide(error);status(control_error);}}
    // The bootstrap process may exit while a child owns the actual swapchain.
    // Only ARC-initialized descendants registered inside this session qualify.
    if(starting_game&&!session.empty()){
        std::vector<std::filesystem::path> paths{session/L"arc.json"};std::error_code error;unsigned seen=0;
        for(const auto& entry:std::filesystem::directory_iterator(session,error)){if(++seen>128)break;if(entry.is_directory()&&entry.path().filename().wstring().starts_with(L"process-"))paths.push_back(entry.path()/L"arc.json");}
        std::uint64_t best=0;bool best_child=false;std::filesystem::path winner;
        for(const auto& path:paths)try{std::ifstream file(path);const auto data=Json::parse(file);const auto pid=data.value("pid",0u);HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!process)continue;const bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);if(!alive)continue;const auto presents=data.value("present_calls",0ull);const bool child=path.parent_path()!=session;if(presents&&((child&&!best_child)||(child==best_child&&presents>best))){best=presents;best_child=child;winner=path;}}catch(...){}
        if(!winner.empty())metrics=winner;
    }
    if(metrics.empty()||!std::filesystem::is_regular_file(metrics))return;
    try{std::ifstream file(metrics);const auto data=Json::parse(file);target_pid=data.value("pid",0u);if(target_pid)SetWindowTextW(pid_edit,std::to_wstring(target_pid).c_str());
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,target_pid);const bool alive=process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;if(process)CloseHandle(process);
        if(!alive){target_pid=0;arc::overlay::update(Json::object(),Json::object(),0);SetWindowTextW(pid_edit,L"");SetWindowTextW(detail_label,L"Подключённого рендера нет.");SetWindowTextW(fps_label,L"Нет кадров от работающего процесса.");status(starting_game&&GetTickCount64()-started_at<15000?L"Стартовый процесс завершился. Ожидание дочернего рендера…":L"Рендер не подключён. Для Steam используйте «Строка для Steam» и запуск из Steam.");return;}
        const auto& automatic=data.at("automatic_session");const auto phase=automatic.value("phase",std::string("off"));
        Json importance=Json::object();try{const auto path=metrics.parent_path()/L"automatic/importance.json";if(std::filesystem::is_regular_file(path)&&std::filesystem::file_size(path)<=2*1024*1024){std::ifstream map_file(path);importance=Json::parse(map_file);}}catch(...){}
        arc::overlay::update(automatic,importance,target_pid);
        if(!busy()&&(diagnostics_pid!=target_pid||diagnostics_requested!=arc::overlay::enabled())){diagnostics_pid=target_pid;diagnostics_requested=arc::overlay::enabled();command(L"--diagnostics "+std::to_wstring(target_pid)+L" "+quote((directory()/L"arc-dx12-probe.dll").wstring())+(diagnostics_requested?L" on":L" off"));}
        if(automatic.contains("current_fps")){wchar_t label[160]{};swprintf_s(label,L"FPS приложения: %.1f     Кадр: %.2f мс",automatic["current_fps"].get<double>(),automatic["current_frame_ms"].get<double>());SetWindowTextW(fps_label,label);}
        std::wstring label=L"Измерение и подбор настроек…";
        if(phase=="target_met")label=automatic.value("active_action",0ull)?L"Цель достигнута. Оптимизация активна.":L"Цель достигнута на исходном качестве.";
        else if(phase=="holding")label=L"Оптимизация активна.";
        else if(phase=="limited")label=L"Пока нет подтверждённого прироста. Наблюдение и поиск продолжаются.";
        else if(phase=="render_metadata_missing")label=L"Кадры получены, но нет данных шейдеров. Закройте игру и выберите «Запустить с ARC».";
        else if(phase=="inactive")label=L"Ожидание кадров DX12.";
        else if(phase=="stopped"){label=automatic.value("restoration_confirmed",false)?L"Оптимизация отключена. Исходный рендер восстановлен.":L"Ожидание подтверждения восстановления…";if(automatic.value("restoration_confirmed",false))target_pid=0;}
        else if(phase=="faulted")label=L"Оптимизация остановлена: "+wide(automatic.value("reason",std::string("ошибка выполнения")));
        else if(phase=="off")label=L"ARC подключён; оптимизация не запущена.";
        const auto coverage=data.value("optimizer_coverage",Json::object());
        if(phase!="stopped"&&phase!="faulted"&&phase!="render_metadata_missing"&&data.value("present_calls",0ull)==0)label=L"ARC загружен. Ожидание первого кадра DX12…";
        if(!control_error.empty())label+=L" "+control_error;status(label);
        const auto details=L"PID "+std::to_wstring(target_pid)+L" · Root signatures: "+std::to_wstring(coverage.value("observed_root_signatures",0u))+L" · Compute PSO: "+std::to_wstring(coverage.value("observed_compute_pipelines",0u));
        SetWindowTextW(detail_label,details.c_str());
    }catch(...){/* a replaced or incomplete diagnostic snapshot is retried */}
}
LRESULT CALLBACK window(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    try{switch(message){
    case WM_CREATE:
        font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        control(hwnd,L"STATIC",L"ARC — автономная оптимизация DX12",0,20,16,680,26);
        list=control(hwnd,L"LISTBOX",L"",LBS_NOTIFY|WS_VSCROLL,20,52,700,180,Games);
        control(hwnd,L"BUTTON",L"Обновить Steam",0,20,245,155,32,Scan);control(hwnd,L"BUTTON",L"Выбрать EXE",0,190,245,150,32,Choose);
        control(hwnd,L"BUTTON",L"Строка для Steam",0,355,245,190,32,SteamOptions);
        control(hwnd,L"BUTTON",L"Карта важности",0,560,245,160,32,Overlay);
        path_label=control(hwnd,L"STATIC",L"Приложение не выбрано",SS_PATHELLIPSIS,20,287,700,25);
        control(hwnd,L"STATIC",L"Аргументы запуска",0,20,325,155,25);arguments=control(hwnd,L"EDIT",L"",ES_AUTOHSCROLL,180,322,540,28,Arguments);
        control(hwnd,L"STATIC",L"Режим ARC",0,20,367,120,25);target_edit=control(hwnd,L"EDIT",L"60",ES_NUMBER,145,363,75,30,Target);control(hwnd,L"BUTTON",L"Изменить цель",0,235,363,155,32,ApplyTarget);
        ShowWindow(target_edit,SW_HIDE);ShowWindow(GetDlgItem(hwnd,ApplyTarget),SW_HIDE);control(hwnd,L"STATIC",L"Максимальный FPS · проверка качества",0,145,367,550,25);
        control(hwnd,L"BUTTON",L"Запустить с ARC",BS_DEFPUSHBUTTON,20,410,210,38,Launch);
        control(hwnd,L"STATIC",L"PID",0,250,419,35,25);pid_edit=control(hwnd,L"EDIT",L"",ES_NUMBER,290,414,95,30,Pid);control(hwnd,L"BUTTON",L"Подключить",0,400,410,140,38,Attach);
        control(hwnd,L"BUTTON",L"Отключить ARC",0,555,410,165,38,Stop);
        fps_label=control(hwnd,L"STATIC",L"FPS появится после подключения",0,20,468,700,27);
        state_label=control(hwnd,L"STATIC",L"",0,20,507,700,70);detail_label=control(hwnd,L"STATIC",L"Отключение: Ctrl+Alt+F10",SS_PATHELLIPSIS,20,588,700,32);
        RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10);RegisterHotKey(hwnd,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9);SetTimer(hwnd,1,500,nullptr);scan();return 0;
    case WM_TIMER:poll();return 0;
    case WM_HOTKEY:if(wparam==2)arc::overlay::toggle();else stop();return 0;
    case WM_COMMAND:
        switch(LOWORD(wparam)){
        case Scan:scan();break;
        case SteamOptions:steam_options(hwnd);break;
        case Overlay:arc::overlay::toggle();break;
        case Games:if(HIWORD(wparam)==LBN_SELCHANGE){const auto index=SendMessageW(list,LB_GETCURSEL,0,0);if(index>=0&&std::size_t(index)<games.size()){selected=game_executable(games[index]);launch_scope=games[index].install_dir;SetWindowTextW(path_label,selected.c_str());}}break;
        case Choose:{wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=hwnd;dialog.lpstrFilter=L"Приложения (*.exe)\0*.exe\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&dialog)){selected=path;launch_scope=selected.parent_path();SetWindowTextW(path_label,selected.c_str());}break;}
        case Launch:launch(false);break;
        case Attach:launch(true);break;
        case Stop:stop();break;
        case ApplyTarget:if(target_pid){const auto fps=target();command(L"--target-fps "+std::to_wstring(target_pid)+L" "+quote((directory()/L"arc-dx12-probe.dll").wstring())+L" "+std::to_wstring(fps));status(L"Изменение целевого FPS…");}break;
        }return 0;
    case WM_CLOSE:if(busy()){status(L"Дождитесь завершения команды ARC перед закрытием.");return 0;}if(target_pid)stop();DestroyWindow(hwnd);return 0;
    case WM_DESTROY:arc::overlay::close();UnregisterHotKey(hwnd,1);UnregisterHotKey(hwnd,2);KillTimer(hwnd,1);if(helper)CloseHandle(helper);if(font)DeleteObject(font);PostQuitMessage(0);return 0;
    }}catch(const std::exception& error){status(wide(error.what()));}
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW cls{};cls.lpfnWndProc=window;cls.hInstance=instance;cls.lpszClassName=L"ARCProductLauncher";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
    const auto hwnd=CreateWindowW(cls.lpszClassName,L"ARC",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,758,675,nullptr,nullptr,instance,nullptr);
    if(!hwnd)return 2;arc::overlay::initialize(instance,hwnd);ShowWindow(hwnd,show);UpdateWindow(hwnd);
    try{steam_launch_arguments();}catch(const std::exception& error){status(wide(error.what()));}
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return int(message.wParam);
}
#else
int main(){return 77;}
#endif
