#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "arc/temporal_visibility.hpp"
#include "arc/visual_importance.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNear = 0.18;

struct Vec3 {
    double x{}, y{}, z{};
};

struct Vec2 {
    double x{}, y{};
    bool valid{};
};

struct RectD {
    double left{}, top{}, right{}, bottom{};

    double width() const noexcept { return std::max(0.0, right - left); }
    double height() const noexcept { return std::max(0.0, bottom - top); }
    double area() const noexcept { return width() * height(); }
};

enum class MotionType : std::uint8_t {
    Static,
    OscillateX,
    OscillateZ,
    Orbit,
    Bob,
};

struct WorldObject {
    arc::ResourceId output_resource{};
    std::uint64_t pipeline{};
    const wchar_t* name{};
    Vec3 base_center{};
    Vec3 center{};
    Vec3 size{};
    double base_yaw{};
    double yaw{};
    COLORREF color{};
    MotionType motion{MotionType::Static};
    double motion_radius{};
    double motion_speed{};
    double motion_phase{};
};

struct ProjectedObject {
    const WorldObject* object{};
    arc::VisualTrackId track_id{};
    RectD rect{};
    std::array<Vec2, 8> corners{};
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

Vec3 rotate_y(const Vec3& p, double yaw) noexcept {
    const double s = std::sin(yaw);
    const double c = std::cos(yaw);
    return {
        p.x * c + p.z * s,
        p.y,
        -p.x * s + p.z * c
    };
}

std::array<Vec3, 8> world_corners(const WorldObject& object) noexcept {
    const double hx = object.size.x * 0.5;
    const double hy = object.size.y * 0.5;
    const double hz = object.size.z * 0.5;
    const std::array<Vec3, 8> local{{
        {-hx, -hy, -hz}, { hx, -hy, -hz},
        { hx, -hy,  hz}, {-hx, -hy,  hz},
        {-hx,  hy, -hz}, { hx,  hy, -hz},
        { hx,  hy,  hz}, {-hx,  hy,  hz},
    }};

    std::array<Vec3, 8> result{};
    for (std::size_t i = 0; i < local.size(); ++i) {
        const auto r = rotate_y(local[i], object.yaw);
        result[i] = {
            object.center.x + r.x,
            object.center.y + r.y,
            object.center.z + r.z
        };
    }
    return result;
}

bool collide(const Player& p, const WorldObject& o, double radius = 0.28) noexcept {
    const double dx = p.x - o.center.x;
    const double dz = p.z - o.center.z;
    const double s = std::sin(-o.yaw);
    const double c = std::cos(-o.yaw);
    const double lx = dx * c + dz * s;
    const double lz = -dx * s + dz * c;
    const double hx = o.size.x * 0.5 + radius;
    const double hz = o.size.z * 0.5 + radius;
    return std::abs(lx) < hx &&
           std::abs(lz) < hz &&
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

const wchar_t* motion_name(MotionType motion) noexcept {
    switch (motion) {
    case MotionType::Static: return L"S";
    case MotionType::OscillateX:
    case MotionType::OscillateZ:
    case MotionType::Orbit:
    case MotionType::Bob: return L"M";
    }
    return L"?";
}

COLORREF importance_color(double value) noexcept {
    value = clamp01(value);
    if (value >= 0.66) return RGB(255, 90, 70);
    if (value >= 0.33) return RGB(255, 210, 70);
    return RGB(80, 230, 120);
}

bool project_point(
    const Vec3& world,
    const Player& player,
    int width,
    int height,
    Vec2& out,
    double* out_depth = nullptr) noexcept {
    const double dx = world.x - player.x;
    const double dz = world.z - player.z;
    const double dy = world.y - player.y;
    const double s = std::sin(player.yaw);
    const double c = std::cos(player.yaw);
    const double camera_x = dx * c - dz * s;
    const double camera_z = dx * s + dz * c;

    if (out_depth) *out_depth = camera_z;
    if (camera_z <= kNear) {
        out = {};
        return false;
    }

    const double fov = 78.0 * kPi / 180.0;
    const double focal = static_cast<double>(width) / (2.0 * std::tan(fov * 0.5));
    out.x = static_cast<double>(width) * 0.5 + camera_x * focal / camera_z;
    out.y = static_cast<double>(height) * 0.5 - dy * focal / camera_z;
    out.valid = true;
    return true;
}

RectD project_object(
    const WorldObject& object,
    const Player& player,
    int width,
    int height,
    std::array<Vec2, 8>& projected_corners,
    double& depth,
    double& screen_x,
    double& screen_y,
    bool& on_screen) noexcept {
    const auto corners = world_corners(object);

    double min_x = static_cast<double>(width);
    double min_y = static_cast<double>(height);
    double max_x = 0.0;
    double max_y = 0.0;
    bool have_projected = false;

    for (std::size_t i = 0; i < corners.size(); ++i) {
        double corner_depth{};
        if (!project_point(corners[i], player, width, height, projected_corners[i], &corner_depth)) {
            continue;
        }
        have_projected = true;
        min_x = std::min(min_x, projected_corners[i].x);
        min_y = std::min(min_y, projected_corners[i].y);
        max_x = std::max(max_x, projected_corners[i].x);
        max_y = std::max(max_y, projected_corners[i].y);
    }

    Vec2 center_screen{};
    if (!project_point(object.center, player, width, height, center_screen, &depth)) {
        depth = -1.0;
    }

    if (!have_projected) {
        on_screen = false;
        screen_x = 0.0;
        screen_y = 0.0;
        return {};
    }

    RectD rect{
        std::clamp(min_x, 0.0, static_cast<double>(width)),
        std::clamp(min_y, 0.0, static_cast<double>(height)),
        std::clamp(max_x, 0.0, static_cast<double>(width)),
        std::clamp(max_y, 0.0, static_cast<double>(height))
    };

    on_screen = rect.area() > 0.0;
    if (center_screen.valid) {
        screen_x = std::clamp((center_screen.x / std::max(1, width)) * 2.0 - 1.0, -1.0, 1.0);
        screen_y = std::clamp((center_screen.y / std::max(1, height)) * 2.0 - 1.0, -1.0, 1.0);
    } else {
        screen_x = 0.0;
        screen_y = 0.0;
    }
    return rect;
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

void draw_oriented_box(HDC dc, const ProjectedObject& p) {
    static constexpr std::array<std::array<int, 4>, 6> faces{{
        {{0,1,2,3}},
        {{4,5,6,7}},
        {{0,1,5,4}},
        {{1,2,6,5}},
        {{2,3,7,6}},
        {{3,0,4,7}},
    }};
    static constexpr std::array<std::array<int, 2>, 12> edges{{
        {{0,1}}, {{1,2}}, {{2,3}}, {{3,0}},
        {{4,5}}, {{5,6}}, {{6,7}}, {{7,4}},
        {{0,4}}, {{1,5}}, {{2,6}}, {{3,7}},
    }};

    HBRUSH fill = CreateSolidBrush(p.object->color);
    HPEN face_pen = CreatePen(PS_SOLID, 1, p.object->color);
    auto old_brush = SelectObject(dc, fill);
    auto old_pen = SelectObject(dc, face_pen);

    for (const auto& face : faces) {
        std::array<POINT, 4> points{};
        bool valid = true;
        for (std::size_t i = 0; i < face.size(); ++i) {
            const auto& corner = p.corners[face[i]];
            if (!corner.valid) {
                valid = false;
                break;
            }
            points[i] = {
                static_cast<LONG>(std::lround(corner.x)),
                static_cast<LONG>(std::lround(corner.y))
            };
        }
        if (valid) {
            Polygon(dc, points.data(), static_cast<int>(points.size()));
        }
    }

    SelectObject(dc, old_brush);
    DeleteObject(fill);
    SelectObject(dc, old_pen);
    DeleteObject(face_pen);

    const COLORREF debug_color = importance_color(p.importance.score);
    HPEN pen = CreatePen(PS_SOLID, p.importance.score > .66 ? 3 : 2, debug_color);
    old_pen = SelectObject(dc, pen);

    for (const auto& edge : edges) {
        const auto& a = p.corners[edge[0]];
        const auto& b = p.corners[edge[1]];
        if (!a.valid || !b.valid) continue;
        MoveToEx(dc, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)), nullptr);
        LineTo(dc, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    SelectObject(dc, old_pen);
    DeleteObject(pen);
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
            {101, 1001, L"Near pillar",   {-2.0, 1.0,  2.0}, {}, {1.0, 2.0, 1.0}, 0.0, {}, RGB(80,150,240)},
            {102, 1001, L"Center crate",  { 1.2, 0.8,  4.0}, {}, {1.4, 1.6, 1.4}, 0.35,{}, RGB(230,150,70)},
            {103, 1002, L"Far wall",      { 0.0, 1.5, 12.0}, {}, {8.0, 3.0, 0.35},0.0, {}, RGB(130,105,175)},
            {104, 1002, L"Left wall",     {-6.0, 1.5,  6.0}, {}, {7.0, 3.0, 0.35},kPi/2.0,{},RGB(105,125,175)},
            {105, 1002, L"Right wall",    { 6.0, 1.5,  6.0}, {}, {7.0, 3.0, 0.35},kPi/2.0,{},RGB(105,125,175)},
            {106, 1003, L"Right tower",   { 4.8, 2.0,  8.0}, {}, {1.3, 4.0, 1.3},-0.2,{}, RGB(210,90,120)},
            {107, 1001, L"Occluded box",  { 1.2, 0.65, 6.4}, {}, {1.1, 1.3, 1.1},0.2, {}, RGB(80,200,220)},
            {108, 1003, L"Thin marker",   {-0.8, 1.0,  9.0}, {}, {0.25,2.0, 0.25},0.0,{}, RGB(220,220,90)},
            {109, 1001, L"Left crate",    {-3.8, 0.7,  5.0}, {}, {1.2, 1.4, 1.2},0.6, {}, RGB(110,200,130)},
            {110, 1003, L"Back column",   { 3.0, 1.5, 11.0}, {}, {0.8, 3.0, 0.8},0.0, {}, RGB(190,110,210)},
            {111, 1004, L"Rear object",   { 0.0, 1.0,-11.0}, {}, {2.0, 2.0, 2.0},0.0, {}, RGB(200,110,220)},
            {112, 1001, L"Corner crate",  {-4.8, 0.6,10.0}, {}, {1.0, 1.2, 1.0},0.8, {}, RGB(120,190,210)},
            {201, 2001, L"Mover X",       { 0.0, 0.75, 7.2}, {}, {0.8, 1.5, 0.8},0.0, {}, RGB(255,120,80), MotionType::OscillateX, 3.0, 0.75, 0.0},
            {202, 2002, L"Mover Z",       {-2.8, 0.6, 8.5}, {}, {0.9, 1.2, 0.9},0.0, {}, RGB(90,235,180), MotionType::OscillateZ, 3.0, 0.55, 1.2},
            {203, 2003, L"Orbit drone",   { 0.0, 2.4, 8.0}, {}, {0.7, 0.45,0.7},0.0, {}, RGB(255,210,80), MotionType::Orbit, 3.8, 0.42, 0.3},
            {204, 2004, L"Bob drone",     { 3.8, 2.0, 5.0}, {}, {0.6, 0.6, 0.6},0.0, {}, RGB(100,200,255), MotionType::Bob, 1.4, 1.1, 0.6},
            {205, 2005, L"Patrol cube",   { 0.0, 0.55,10.5}, {}, {0.9, 1.1, 0.9},0.0, {}, RGB(255,110,190), MotionType::OscillateX, 4.2, 0.38, 2.0},
            {206, 2006, L"Side mover",    { 4.5, 0.65, 4.0}, {}, {0.8, 1.3, 0.8},0.0, {}, RGB(180,240,100), MotionType::OscillateZ, 2.4, 0.68, 0.8},
        };

        for (auto& object : objects_) {
            object.center = object.base_center;
            object.yaw = object.base_yaw;
        }
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
            elapsed_ += dt;

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
    double elapsed_{};
    POINT last_mouse_{};
    bool have_mouse_{};

    void animate_world() {
        for (auto& object : objects_) {
            object.center = object.base_center;
            object.yaw = object.base_yaw;

            const double t = elapsed_ * object.motion_speed + object.motion_phase;
            switch (object.motion) {
            case MotionType::Static:
                break;
            case MotionType::OscillateX:
                object.center.x += std::sin(t) * object.motion_radius;
                object.yaw += 0.35 * std::sin(t * 0.7);
                break;
            case MotionType::OscillateZ:
                object.center.z += std::sin(t) * object.motion_radius;
                object.yaw += 0.45 * std::sin(t * 0.6);
                break;
            case MotionType::Orbit:
                object.center.x += std::cos(t) * object.motion_radius;
                object.center.z += std::sin(t) * object.motion_radius;
                object.yaw = -t + kPi * 0.5;
                break;
            case MotionType::Bob:
                object.center.y += std::sin(t * 1.7) * object.motion_radius;
                object.center.x += std::sin(t * 0.55) * 0.7;
                object.yaw += t;
                break;
            }
        }
    }

    void update(double dt) {
        if (key_down(VK_ESCAPE)) {
            running_ = false;
            return;
        }

        animate_world();

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
            if (object.motion != MotionType::Static) continue;
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
            p.rect = project_object(
                object,
                player_,
                width,
                height,
                p.corners,
                p.depth,
                p.screen_x,
                p.screen_y,
                p.on_screen);
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
            if (!p.on_screen || p.depth <= kNear) {
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
            arc::WorkObservation work{};
            work.kind = arc::GpuWorkKind::Draw;
            work.pipeline = p.object->pipeline;
            work.items = 36;
            work.raster = {
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                {0.0, 0.0, static_cast<double>(width), static_cast<double>(height)},
                {p.rect.left, p.rect.top, p.rect.width(), p.rect.height()},
                p.on_screen
            };
            work.accesses.push_back({
                p.object->output_resource,
                true,
                arc::AccessEvidence::Observed,
                true
            });
            work.bindings_complete = true;

            const auto fingerprint = arc::make_visual_track_fingerprint(work);
            p.track_id = fingerprint.id;

            arc::VisibilityObservation observation{};
            observation.id = p.track_id;
            observation.frame = frame_;
            observation.local_coverage_upper = p.upper_coverage;
            observation.visible_coverage = p.visible_coverage;
            observation.present_reachable = p.on_screen && p.depth > kNear;
            observation.confidence = std::min(0.96, fingerprint.confidence);
            temporal_.observe(observation);

            if (const auto* state = temporal_.find(p.track_id)) {
                p.temporal = *state;
                arc::VisualImportanceHint hint{};
                if (p.on_screen) {
                    hint.screen_x = p.screen_x;
                    hint.screen_y = p.screen_y;
                }
                hint.composition_relevance = 1.0;
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
            if (!p.on_screen || p.depth <= kNear) continue;

            RECT r{
                static_cast<LONG>(p.rect.left),
                static_cast<LONG>(p.rect.top),
                static_cast<LONG>(p.rect.right),
                static_cast<LONG>(p.rect.bottom)
            };

            draw_oriented_box(dc, p);

            wchar_t label[260]{};
            swprintf_s(
                label,
                L"%s %s  IMP %.2f  vis %.2f%%  pred8 %.2f%%  %s",
                motion_name(p.object->motion),
                p.object->name,
                p.importance.score,
                p.temporal.estimated_visible_coverage * 100.0,
                p.temporal.predicted_8f * 100.0,
                phase_name(p.temporal.phase));
            const int tx = std::clamp(r.left, 4L, static_cast<LONG>(std::max(4, width - 520)));
            const int ty = std::clamp(r.top - 18, 4L, static_cast<LONG>(std::max(4, height - 22)));
            draw_text(dc, tx, ty, importance_color(p.importance.score), label);
        }

        HFONT font = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FF_DONTCARE, L"Consolas");
        auto old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);

        const int row_h = 14;
        const int panel_h = 90 + row_h * static_cast<int>(projected_.size()) + 10;
        HBRUSH panel = CreateSolidBrush(RGB(10, 12, 16));
        RECT panel_rect{10, 10, std::min(width - 10, 690), std::min(height - 10, panel_h)};
        FillRect(dc, &panel_rect, panel);
        DeleteObject(panel);

        wchar_t header[256]{};
        swprintf_s(
            header,
            L"ARC Mega E debugger | FPS %.1f | objects %zu | M=moving S=static",
            fps_,
            projected_.size());
        draw_text(dc, 18, 16, RGB(235, 240, 250), header);
        draw_text(dc, 18, 32, RGB(185, 195, 210), L"WASD move | Shift sprint | arrows / RMB mouse look | Esc exit");
        draw_text(dc, 18, 48, RGB(185, 195, 210), L"World-space oriented boxes: walls no longer billboard toward the camera.");
        draw_text(dc, 18, 64, RGB(185, 195, 210), L"TY NAME            PHASE     VIS%   P8%   IMP   CONF");

        auto sorted = projected_;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            if (a.object->output_resource != b.object->output_resource) {
                return a.object->output_resource < b.object->output_resource;
            }
            return a.object->pipeline < b.object->pipeline;
        });

        int y = 80;
        for (const auto& p : sorted) {
            wchar_t line[320]{};
            swprintf_s(
                line,
                L"%s  %-15s %-9s %5.2f %5.2f %5.3f %5.2f",
                motion_name(p.object->motion),
                p.object->name,
                phase_name(p.temporal.phase),
                p.temporal.estimated_visible_coverage * 100.0,
                p.temporal.predicted_8f * 100.0,
                p.importance.score,
                p.importance.confidence);
            draw_text(dc, 18, y, importance_color(p.importance.score), line);
            y += row_h;
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
        1500,
        900,
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
