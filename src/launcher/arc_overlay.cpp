#include "arc_overlay.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
namespace arc::overlay {
namespace {
HWND window{},owner{};HFONT font{},small_font{};bool visible{},standalone{};
nlohmann::json status,map;
std::wstring label;
std::wstring wide(const std::string& text){if(text.empty())return {};const auto size=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);std::wstring value(size,0);MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),value.data(),size);return value;}
void line(HDC dc,int y,const std::wstring& text,COLORREF color=RGB(235,240,245),bool compact=false){SelectObject(dc,compact?small_font:font);SetTextColor(dc,color);RECT r{14,y,506,y+28};DrawTextW(dc,text.c_str(),int(text.size()),&r,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);}
void paint(HDC dc){
    RECT area{};GetClientRect(window,&area);HBRUSH background=CreateSolidBrush(RGB(18,23,31));FillRect(dc,&area,background);DeleteObject(background);SetBkMode(dc,TRANSPARENT);
    line(dc,10,L"ARC · Ctrl+Alt+F9 — карта / скрыть",RGB(110,210,245));
    try{const auto fps=status.value("current_fps",0.0),ms=status.value("current_frame_ms",0.0);wchar_t value[128]{};swprintf_s(value,L"%.1f FPS   |   %.2f мс   |   Политика %llu",fps,ms,status.value("active_action",status.value("retained",0ull)));line(dc,38,value);
        const auto phase=status.value("phase",std::string("off"));std::wstring text=L"Измерение и проверка";
        if(phase=="holding")text=L"Принятое изменение активно";else if(phase=="limited")text=L"Подтверждённого прироста пока нет";else if(phase=="stopped"||phase=="off")text=L"Оптимизация отключена";else if(phase=="faulted")text=L"Остановлено: "+wide(status.value("reason",std::string{}));else if(phase=="inactive")text=L"Ожидание кадров";
        line(dc,66,text,RGB(230,220,155),true);
        if(map.empty()||!map.contains("tiles")){line(dc,115,L"Карта прохода пока не получена.",RGB(175,185,195));line(dc,145,L"Неизвестные области не считаются маловажными.",RGB(175,185,195),true);}
        else{const auto nx=map.value("tiles_x",0u),ny=map.value("tiles_y",0u);const auto& tiles=map.at("tiles");
            if(!nx||!ny||std::uint64_t(nx)*ny>8192||tiles.size()!=std::uint64_t(nx)*ny)throw std::runtime_error("map dimensions");
            const auto frame=map.value("frame",0ull),current=status.value("present_samples",0ull);const auto age=current>=frame?current-frame:0;
            line(dc,94,L"Последний снимок · проход "+std::to_wstring(map.value("pipeline",0ull))+L" · возраст "+std::to_wstring(age)+L" кадров",RGB(175,190,205),true);
            std::vector<std::uint32_t> pixels;pixels.reserve(tiles.size());const auto threshold=std::max(.0001,map.value("edge_threshold",.5));
            for(const auto& tile:tiles){if(!tile.is_array()||tile.size()!=3)throw std::runtime_error("map cell");const double importance=tile[0],confidence=tile[2];const unsigned mode=tile[1];
                std::uint32_t rgb=0x606875;if(confidence>0&&std::isfinite(importance)){const auto intensity=unsigned(std::clamp(importance/threshold,0.0,1.0)*110);rgb=mode?(0x207050u+(intensity<<8)):(0x903028u+(intensity<<16));}pixels.push_back(rgb);}
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=LONG(nx);info.bmiHeader.biHeight=-LONG(ny);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            const double scale=std::min(492.0/nx,190.0/ny);const int map_width=std::max(1,int(nx*scale)),map_height=std::max(1,int(ny*scale));
            SetStretchBltMode(dc,COLORONCOLOR);StretchDIBits(dc,14+(492-map_width)/2,122+(190-map_height)/2,map_width,map_height,0,0,int(nx),int(ny),pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            line(dc,316,L"На снимке: зелёный — упрощение, красный — полный",RGB(205,220,205),true);
            line(dc,339,L"Серый: нет уверенности. Карта в координатах прохода.",RGB(185,195,205),true);
            line(dc,362,map.value("final_screen_correspondence",false)?L"Координаты backbuffer; центр — приоритет, не взгляд.":L"Связь с экраном не доказана; это не карта объектов.",RGB(235,195,125),true);
        }
        line(dc,390,standalone?L"Отдельная диагностика: эксклюзивный Fullscreen":L"Клик проходит в игру · Ctrl+Alt+F10 — отключить ARC",RGB(155,175,190),true);
    }catch(...){line(dc,115,L"Диагностические данные недоступны.",RGB(235,150,120));}
}
LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==WM_NCHITTEST&&!standalone)return HTTRANSPARENT;
    if(message==WM_MOUSEACTIVATE&&!standalone)return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_CLOSE){visible=false;ShowWindow(hwnd,SW_HIDE);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);HDC buffer=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom));auto old=SelectObject(buffer,bitmap);paint(buffer);BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);EndPaint(hwnd,&ps);return 0;}
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
}
void initialize(HINSTANCE instance,HWND parent){owner=parent;WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=proc;cls.lpszClassName=L"ARCImportanceOverlay";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
    window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"ARC — карта важности",WS_POPUP,20,60,520,425,parent,nullptr,instance,nullptr);
    SetLayeredWindowAttributes(window,0,225,LWA_ALPHA);font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");small_font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");}
void toggle(){visible=!visible;if(!visible&&window)ShowWindow(window,SW_HIDE);}
bool enabled()noexcept{return visible;}
void update(const nlohmann::json& automatic,const nlohmann::json& importance,DWORD pid){
    status=automatic;map=importance;if(!window||!visible)return;
    HWND target=reinterpret_cast<HWND>(std::uintptr_t(automatic.value("window_handle",0ull)));DWORD actual{};if(target)GetWindowThreadProcessId(target,&actual);
    if(!target||actual!=pid||!IsWindow(target)||IsIconic(target)){ShowWindow(window,SW_HIDE);return;}
    const bool separate=automatic.value("exclusive_fullscreen",false);
    if(separate!=standalone){standalone=separate;SetWindowLongPtrW(window,GWL_EXSTYLE,WS_EX_LAYERED|WS_EX_TOOLWINDOW|(separate?0:WS_EX_TRANSPARENT|WS_EX_NOACTIVATE));SetWindowLongPtrW(window,GWL_STYLE,separate?WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU:WS_POPUP);}
    POINT position{16,16};ClientToScreen(separate?owner:target,&position);SetWindowPos(window,separate?HWND_NOTOPMOST:HWND_TOPMOST,position.x,position.y,520,425,SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_FRAMECHANGED);InvalidateRect(window,nullptr,FALSE);
}
void close(){if(window)DestroyWindow(window);window=nullptr;if(font)DeleteObject(font);if(small_font)DeleteObject(small_font);font=small_font=nullptr;}
}
