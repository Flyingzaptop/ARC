#ifdef _WIN32

#include "arc/temporal_visibility.hpp"
#include "arc/visual_importance.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double x{}, y{}, z{};
};

struct RectD {
    double left{}, top{}, right{}, bottom{};

    double width() const noexcept { return std::max(0.0, right - left); }
    double height() const noexcept { return std::max(0.0, bottom - top); }
    double area() const noexcept { return width() * height(); }
};

struct WorldObject {
    arc::VisualTrackId id{};
    const wchar_t* name{};
    Vec3 center{};
    Vec3 size{};
    COLORREF color{};
};

struct ProjectedObject {
    const WorldObject* object{};
    RectD rect{};
    double depth{};
    double upper_coverage{};
    double visible_coverage{};
    double screen_x{};
    double screen_y{};
    bool on_screen{};
    arc::TemporalVisibilityState temporal{};
    arc::VisualImportance importance{};
};

struct Player {
    double x{0.0};
    double z{-7.0};
    double y{1.65};
    double yaw{0.0};
};

bool key_down(int key) noexcept {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

double intersection_area(const RectD& a, const RectD& b) noexcept {
    const double left = std::max(a.left, b.left);
    const double top = std::max(a.top, b.top);
    const double right = std::min(a.right, b.right);
    const double bottom = std::min(a.bottom, b.bottom);
    return std::max(0.0, right - left) * std::max(0.0, bottom - top);
}

bool collide(const Player& p, const WorldObject& o, double radius = 0.28) noexcept {
    const double hx = o.size.x * 0.5 + radius;
    const double hz = o.size.z * 0.5 + radius;
    return std::abs(p.x - o.center.x) < hx &&
           std::abs(p.z - o.center.z) < hz &&
           p.y > o.center.y - o.size.y * 0.5 - 0.5 &&
           p.y < o.center.y + o.size.y * 0.5 + 1.0;
}

const wchar_t* phase_name(arc::VisibilityPhase phase) noexcept {
    switch (phase) {
    case arc::VisibilityPhase::Hidden: return L"HIDDEN";
    case arc::VisibilityPhase::Visible: return L"VISIBLE";
    case arc::VisibilityPhase::Entering: return L"ENTERING";
    case arc::VisibilityPhase::Leaving: return L"LEAVING";
    case arc::VisibilityPhase::Unknown: return L"UNKNOWN";
    }
    return L"UNKNOWN";
}

COLORREF importance_color(double value) noexcept {
    value = clamp01(value);
    if (value >= 0.66) return RGB(255, 90, 70);
    if (value >= 0.33) return RGB(255, 210, 70);
    return RGB(80, 230, 120);
}

RectD project_object(
    const WorldObject& object,
    const Player& player,
    int width,
    int height,
    double& depth,
    double& screen_x,
    double& screen_y,
    bool& on_screen) noexcept {
    const double dx = object.center.x - player.x;
    const double dz = object.center.z - player.z;
    const double s = std::sin(player.yaw);
    const double c = std::cos(player.yaw);

    const double camera_x = dx * c - dz * s;
    const double camera_z = dx * s + dz * c;
    const double camera_y = object.center.y - player.y;
    depth = camera_z;

    if (camera_z <= 0.18) {
        on_screen = false;
        screen_x = 0.0;
        screen_y = 0.0;
        return {};
    }

    const double fov = 78.0 * kPi / 180.0;
    const double focal = static_cast<double>(width) / (2.0 * std::tan(fov * 0.5));
    const double sx = static_cast<double>(width) * 0.5 + camera_x * focal / camera_z;
    const double sy = static_cast<double>(height) * 0.5 - camera_y * focal / camera_z;
    const double apparent_w = std::max(object.size.x, object.size.z) * focal / camera_z;
    const double apparent_h = object.size.y * focal / camera_z;

    RectD rect{
        sx - apparent_w * 0.5,
        sy - apparent_h * 0.5,
        sx + apparent_w * 0.5,
        sy + apparent_h * 0.5
    };

    RectD clipped{
        std::clamp(rect.left, 0.0, static_cast<double>(width)),
        std::clamp(rect.top, 0.0, static_cast<double>(height)),
        std::clamp(rect.right, 0.0, static_cast<double>(width)),
        std::clamp(rect.bottom, 0.0, static_cast<double>(height))
    };

    on_screen = clipped.area() > 0.0;
    screen_x = std::clamp((sx / std::max(1, width)) * 2.0 - 1.0, -1.0, 1.0);
    screen_y = std::clamp((sy / std::max(1, height)) * 2.0 - 1.0, -1.0, 1.0);
    return clipped;
}

void draw_text(HDC dc, int x, int y, COLORREF color, const wchar_t* text) {
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
}

void draw_floor_grid(HDC dc, const RECT& client, const Player& player) {
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    const int horizon = h / 2 + 35;

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(55, 60, 70));
    auto old = SelectObject(dc, pen);

    MoveToEx(dc, 0, horizon, nullptr);
    LineTo(dc, w, horizon);

    for (int i = -8; i <= 8; ++i) {
        const int bx = w / 2 + i * 70;
        MoveToEx(dc, w / 2, horizon, nullptr);
        LineTo(dc, bx, h);
    }
    for (int i = 1; i <= 12; ++i) {
        const double t = static_cast<double>(i) / 12.0;
        const int y = horizon + static_cast<int>((h - horizon) * t * t);
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, w, y);
    }

    SelectObject(dc, old);
    DeleteObject(pen);

    wchar_t pos[160]{};
    swprintf_s(pos, L"POS x %.2f  z %.2f  yaw %.1f deg", player.x, player.z, player.yaw * 180.0 / kPi);
    draw_text(dc, 18, h - 28, RGB(160, 170, 185), pos);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

class Demo final {
public:
    explicit Demo(HWND hwnd) : hwnd_(hwnd) {
        objects_ = {
            {101, L"Near pillar", {-2.0, 1.0, 2.0}, {1.0, 2.0, 1.0}, RGB(80, 150, 240)},
            {102, L"Center crate", {1.2, 0.8, 4.0}, {1.4, 1.6, 1.4}, RGB(230, 150, 70)},
            {103, L"Far wall", {0.0, 1.5, 12.0}, {8.0, 3.0, 0.7}, RGB(130, 105, 175)},
            {104, L"Left small", {-5.0, 0.7, 7.0}, {0.8, 1.4, 0.8}, RGB(90, 200, 160)},
            {105, L"Right tower", {5.5, 2.0, 8.0}, {1.3, 4.0, 1.3}, RGB(210, 90, 120)},
            {106, L"Occluded box", {1.2, 0.65, 6.4}, {1.1, 1.3, 1.1}, RGB(80, 200, 220)},
            {107, L"Rear object", {0.0, 1.0, -11.0}, {2.0, 2.0, 2.0}, RGB(200, 110, 220)},
            {108, L"Thin marker", {-0.8, 1.0, 9.0}, {0.25, 2.0, 0.25}, RGB(220, 220, 90)},
        };
    }

    int run() {
        using clock = std::chrono::steady_clock;
        auto previous = clock::now();
        auto fps_start = previous;
        int fps_frames = 0;

        while (running_) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running_ = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_) break;

            const auto now = clock::now();
            const double dt = std::clamp(
                std::chrono::duration<double>(now - previous).count(),
                0.0,
                0.05);
            previous = now;

            update(dt);
            render();

            ++fps_frames;
            const double fps_elapsed = std::chrono::duration<double>(now - fps_start).count();
            if (fps_elapsed >= 0.5) {
                fps_ = static_cast<double>(fps_frames) / fps_elapsed;
                fps_frames = 0;
                fps_start = now;
            }
            Sleep(1);
        }
        return 0;
    }

private:
    HWND hwnd_{};
    bool running_{true};
    Player player_{};
    std::vector<WorldObject> objects_{};
    arc::TemporalVisibilityModel temporal_{};
    arc::VisualImportanceModel importance_{};
    std::vector<ProjectedObject> projected_{};
    std::uint64_t frame_{};
    double fps_{};
    POINT last_mouse_{};
    bool have_mouse_{};

    void update(double dt) {
        if (key_down(VK_ESCAPE)) {
            running_ = false;
            return;
        }

        double yaw_delta = 0.0;
        if (key_down(VK_LEFT)) yaw_delta -= 1.8 * dt;
        if (key_down(VK_RIGHT)) yaw_delta += 1.8 * dt;

        if (key_down(VK_RBUTTON)) {
            POINT p{};
            GetCursorPos(&p);
            if (have_mouse_) yaw_delta += static_cast<double>(p.x - last_mouse_.x) * 0.0035;
            last_mouse_ = p;
            have_mouse_ = true;
        } else {
            have_mouse_ = false;
        }
        player_.yaw += yaw_delta;

        const double forward_x = std::sin(player_.yaw);
        const double forward_z = std::cos(player_.yaw);
        const double right_x = std::cos(player_.yaw);
        const double right_z = -std::sin(player_.yaw);
        const double speed = (key_down(VK_SHIFT) ? 6.0 : 3.0) * dt;

        double dx = 0.0;
        double dz = 0.0;
        if (key_down('W')) { dx += forward_x * speed; dz += forward_z * speed; }
        if (key_down('S')) { dx -= forward_x * speed; dz -= forward_z * speed; }
        if (key_down('A')) { dx -= right_x * speed; dz -= right_z * speed; }
        if (key_down('D')) { dx += right_x * speed; dz += right_z * speed; }

        Player candidate = player_;
        candidate.x += dx;
        candidate.z += dz;
        bool blocked = false;
        for (const auto& object : objects_) {
            if (collide(candidate, object)) {
                blocked = true;
                break;
            }
        }
        if (!blocked) player_ = candidate;

        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);

        projected_.clear();
        projected_.reserve(objects_.size());

        for (const auto& object : objects_) {
            ProjectedObject p{};
            p.object = &object;
            p.rect = project_object(object, player_, width, height, p.depth, p.screen_x, p.screen_y, p.on_screen);
            const double screen_area = static_cast<double>(width) * static_cast<double>(height);
            p.upper_coverage = p.on_screen ? clamp01(p.rect.area() / screen_area) : 0.0;
            projected_.push_back(p);
        }

        std::vector<std::size_t> depth_order(projected_.size());
        for (std::size_t i = 0; i < depth_order.size(); ++i) depth_order[i] = i;
        std::sort(depth_order.begin(), depth_order.end(), [&](std::size_t a, std::size_t b) {
            return projected_[a].depth < projected_[b].depth;
        });

        std::vector<RectD> nearer;
        for (const auto index : depth_order) {
            auto& p = projected_[index];
            if (!p.on_screen || p.depth <= 0.18) {
                p.visible_coverage = 0.0;
                continue;
            }
            double visible_area = p.rect.area();
            for (const auto& occluder : nearer) {
                visible_area -= intersection_area(p.rect, occluder);
                if (visible_area <= 0.0) {
                    visible_area = 0.0;
                    break;
                }
            }
            const double screen_area = static_cast<double>(width) * static_cast<double>(height);
            p.visible_coverage = clamp01(visible_area / screen_area);
            nearer.push_back(p.rect);
        }

        ++frame_;
        for (auto& p : projected_) {
            arc::VisibilityObservation observation{};
            observation.id = p.object->id;
            observation.frame = frame_;
            observation.local_coverage_upper = p.upper_coverage;
            observation.visible_coverage = p.visible_coverage;
            observation.present_reachable = p.on_screen && p.depth > 0.18;
            observation.confidence = 0.96;
            temporal_.observe(observation);

            if (const auto* state = temporal_.find(p.object->id)) {
                p.temporal = *state;
                arc::VisualImportanceHint hint{};
                if (p.on_screen) {
                    hint.screen_x = p.screen_x;
                    hint.screen_y = p.screen_y;
                }
                hint.composition_relevance = 1.0;
                // Deliberately omit semantic/perceptual object labels. Unknown
                // sensitivity defaults conservatively in the Stage 19 model.
                p.importance = importance_.evaluate(*state, hint);
            }
        }
    }

    void render() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);

        HDC window_dc = GetDC(hwnd_);
        HDC dc = CreateCompatibleDC(window_dc);
        HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
        auto old_bitmap = SelectObject(dc, bitmap);

        HBRUSH background = CreateSolidBrush(RGB(18, 21, 27));
        FillRect(dc, &client, background);
        DeleteObject(background);

        RECT sky{0, 0, width, height / 2 + 35};
        HBRUSH sky_brush = CreateSolidBrush(RGB(28, 36, 50));
        FillRect(dc, &sky, sky_brush);
        DeleteObject(sky_brush);

        draw_floor_grid(dc, client, player_);

        std::vector<std::size_t> painter(projected_.size());
        for (std::size_t i = 0; i < painter.size(); ++i) painter[i] = i;
        std::sort(painter.begin(), painter.end(), [&](std::size_t a, std::size_t b) {
            return projected_[a].depth > projected_[b].depth;
        });

        for (const auto index : painter) {
            const auto& p = projected_[index];
            if (!p.on_screen || p.depth <= 0.18) continue;

            RECT r{
                static_cast<LONG>(p.rect.left),
                static_cast<LONG>(p.rect.top),
                static_cast<LONG>(p.rect.right),
                static_cast<LONG>(p.rect.bottom)
            };

            HBRUSH fill = CreateSolidBrush(p.object->color);
            FillRect(dc, &r, fill);
            DeleteObject(fill);

            const COLORREF debug_color = importance_color(p.importance.score);
            HPEN pen = CreatePen(PS_SOLID, p.importance.score > .66 ? 3 : 2, debug_color);
            auto old_pen = SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, old_pen);
            DeleteObject(pen);

            wchar_t label[260]{};
            swprintf_s(
                label,
                L"%s  IMP %.2f  vis %.2f%%  pred8 %.2f%%  %s",
                p.object->name,
                p.importance.score,
                p.temporal.estimated_visible_coverage * 100.0,
                p.temporal.predicted_8f * 100.0,
                phase_name(p.temporal.phase));
            const int tx = std::clamp(r.left, 4L, static_cast<LONG>(std::max(4, width - 500)));
            const int ty = std::clamp(r.top - 20, 4L, static_cast<LONG>(std::max(4, height - 24)));
            draw_text(dc, tx, ty, debug_color, label);
        }

        HFONT font = CreateFontW(
            -17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FF_DONTCARE, L"Consolas");
        auto old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);

        HBRUSH panel = CreateSolidBrush(RGB(10, 12, 16));
        RECT panel_rect{12, 12, std::min(width - 12, 610), std::min(height - 12, 230)};
        FillRect(dc, &panel_rect, panel);
        DeleteObject(panel);

        wchar_t header[256]{};
        swprintf_s(header, L"ARC Mega E visual debugger   FPS %.1f", fps_);
        draw_text(dc, 24, 22, RGB(235, 240, 250), header);
        draw_text(dc, 24, 45, RGB(185, 195, 210), L"WASD move | Shift sprint | arrows / RMB mouse look | Esc exit");
        draw_text(dc, 24, 68, RGB(185, 195, 210), L"Boxes = ARC tracked contribution. Border = Visual Importance.");
        draw_text(dc, 24, 91, RGB(185, 195, 210), L"vis = estimated visible screen coverage | pred8 = 8-frame prediction");

        int y = 120;
        auto sorted = projected_;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.importance.score > b.importance.score;
        });
        for (const auto& p : sorted) {
            wchar_t line[280]{};
            swprintf_s(
                line,
                L"#%llu %-14s imp %.3f  conf %.2f  %-8s",
                static_cast<unsigned long long>(p.object->id),
                p.object->name,
                p.importance.score,
                p.importance.confidence,
                phase_name(p.temporal.phase));
            draw_text(dc, 24, y, importance_color(p.importance.score), line);
            y += 19;
            if (y > panel_rect.bottom - 20) break;
        }

        SelectObject(dc, old_font);
        DeleteObject(font);

        BitBlt(window_dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);

        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(hwnd_, window_dc);
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"ARC-Mega-E-Visual-FPS";
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ARC Mega E - Visibility & Visual Importance Debug FPS",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1400,
        820,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd) return 2;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    Demo demo(hwnd);
    return demo.run();
}

#else
int main() { return 77; }
#endif
