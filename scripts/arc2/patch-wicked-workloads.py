"""Deterministic Wicked scene selection, camera route, and bounded exit only."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAMPLES = ROOT / "build/arc2-testbeds/WickedEngine/Samples/Tests"
ENGINE = ROOT / "build/arc2-testbeds/WickedEngine/WickedEngine"


def patch(path: Path, old: str, new: str) -> None:
    content = path.read_text(encoding="utf-8-sig")
    if new in content:
        print(f"unchanged {path}")
        return
    if content.count(old) != 1:
        raise RuntimeError(f"Expected one anchor in {path}: {old!r}")
    path.write_text(content.replace(old, new), encoding="utf-8")
    print(f"patched {path}")


tests = SAMPLES / "Tests.cpp"
random = ENGINE / "wiRandom.cpp"
patch(random, '#include <time.h>', '#include <time.h>\n#include <cstdlib>')
patch(random, '\tstatic thread_local RNG rng(time(nullptr));', '''\tstatic thread_local RNG rng([] {
        const char* value = std::getenv("ARC2_WICKED_SEED");
        return value && *value ? uint64_t(std::strtoull(value, nullptr, 10)) : uint64_t(time(nullptr));
    }());''')
patch(ENGINE / "wiApplication.cpp", '\t\tdeltaTime = clamp(deltaTime, 0.0f, 0.5f);', '''\t\tdeltaTime = clamp(deltaTime, 0.0f, 0.5f);
        if (GetEnvironmentVariableA("ARC2_WICKED_FIXED_DT", nullptr, 0) > 0)
            deltaTime = 1.0f / 60.0f; // Deterministic testbed simulation step only.''')
header = SAMPLES / "Tests.h"
patch(header, '\tvoid Initialize() override;', '''\tvoid Initialize() override;
    bool SaveArc2Capture(const char* path);''')
patch(tests, 'void Tests::Initialize()\n{', '''bool Tests::SaveArc2Capture(const char* path)
{
    return wi::helper::saveTextureToFile(renderer.GetRenderResult3D(), path);
}

void Tests::Initialize()
{''')
patch(tests, '\ttestSelector.SetSelected(0);', '''\tchar arc2_scene[32]{};
\tGetEnvironmentVariableA("ARC2_WICKED_SCENE", arc2_scene, sizeof(arc2_scene));
\tif (strcmp(arc2_scene, "instances") == 0)
\t\ttestSelector.SetSelectedByUserdata(INSTANCESTEST);
\telse if (strcmp(arc2_scene, "visibility") == 0)
\t\ttestSelector.SetSelectedByUserdata(SHADOWSTEST);
\telse
\t\ttestSelector.SetSelected(0);''')
patch(tests, '\tgui.AddWidget(&testSelector);', '''\tgui.AddWidget(&testSelector);
    if (GetEnvironmentVariableA("ARC2_WICKED_QUALITY_FIXED", nullptr, 0) > 0) {
        wi::renderer::SetTemporalAAEnabled(false);
        setEyeAdaptionEnabled(false);
        setExposure(1.0f);
    }''')
patch(tests, '\t\tfloat sec = (float)timer.elapsed_seconds();', '''\t\tfloat sec = (float)timer.elapsed_seconds();
        if (GetEnvironmentVariableA("ARC2_WICKED_FIXED_DT", nullptr, 0) > 0) {
            static unsigned long long arc2_quality_frame = 0;
            sec = float(arc2_quality_frame++) / 60.0f;
        }''')
old_camera = '''    if (userdata == SHADOWSTEST) {
        // Deterministic camera translation; no ARC metadata or callbacks.
        static unsigned arc2_frame = 0;
        const float phase = float(arc2_frame++ % 600) * 0.012f;
        TransformComponent route;
        route.Translate(XMFLOAT3(std::sin(phase) * 1.5f, 2.0f,
                                 -4.5f + std::cos(phase) * 0.5f));
        route.UpdateTransform();
        wi::scene::GetCamera().TransformCamera(route);
    }
    RenderPath3D::Update(dt);'''
new_camera = '''    if (userdata == SHADOWSTEST) {
        // Same camera route versus wall time in performance; fixed dt in quality.
        static float arc2_phase = 0.0f;
        const float phase = arc2_phase;
        arc2_phase += dt * 0.72f;
        if (arc2_phase >= 6.2831853f) arc2_phase -= 6.2831853f;
        TransformComponent route;
        route.Translate(XMFLOAT3(std::sin(phase) * 1.5f, 2.0f,
                                 -4.5f + std::cos(phase) * 0.5f));
        route.UpdateTransform();
        wi::scene::GetCamera().TransformCamera(route);
    }
    RenderPath3D::Update(dt);'''
if old_camera in tests.read_text(encoding="utf-8-sig"):
    patch(tests, old_camera, new_camera)
else:
    patch(tests, '    RenderPath3D::Update(dt);', new_camera)
main = SAMPLES / "main_Windows.cpp"
loader = (ROOT / "include/arc/arc2/bootstrap_loader.hpp").as_posix()
patch(main, '#include "stdafx.h"', '#include "stdafx.h"\n' + f'#include "{loader}"')
old_frames = '''\tchar arc2_frames_text[32]{};
\tGetEnvironmentVariableA("ARC2_WICKED_FRAMES", arc2_frames_text, sizeof(arc2_frames_text));
\tconst unsigned arc2_max_frames = static_cast<unsigned>(atoi(arc2_frames_text));
\tunsigned arc2_frames = 0;
\tMSG msg = { 0 };'''
new_frames = '''\tchar arc2_frames_text[32]{};
\tGetEnvironmentVariableA("ARC2_WICKED_FRAMES", arc2_frames_text, sizeof(arc2_frames_text));
\tconst unsigned arc2_max_frames = static_cast<unsigned>(atoi(arc2_frames_text));
\tunsigned arc2_frames = 0;
    char arc2_sequence_dir[32768]{};
    GetEnvironmentVariableA("ARC2_WICKED_CAPTURE_DIR", arc2_sequence_dir, sizeof(arc2_sequence_dir));
    bool arc2_sequence_ok = true;
\tMSG msg = { 0 };'''
if old_frames in main.read_text(encoding="utf-8-sig"):
    patch(main, old_frames, new_frames)
else:
    patch(main, '\tMSG msg = { 0 };', new_frames)
old_exit = '''\t\t\ttests.Run();
\t\t\tif (arc2_max_frames && ++arc2_frames >= arc2_max_frames)
\t\t\t\tPostQuitMessage(0);'''
new_exit = '''\t\t\ttests.Run();
            if (arc2_sequence_dir[0] && arc2_frames >= 179 && arc2_frames < 204) {
                char arc2_path[32768]{};
                sprintf_s(arc2_path, "%s/frame%03u.png", arc2_sequence_dir, arc2_frames - 179);
                arc2_sequence_ok &= tests.SaveArc2Capture(arc2_path);
            }
\t\t\tif (arc2_max_frames && ++arc2_frames >= arc2_max_frames)
\t\t\t{
\t\t\t\tchar arc2_capture[32768]{};
\t\t\t\tGetEnvironmentVariableA("ARC2_WICKED_SCREENSHOT", arc2_capture, sizeof(arc2_capture));
\t\t\t\tconst bool arc2_capture_ok = !arc2_capture[0] || tests.SaveArc2Capture(arc2_capture);
\t\t\t\tarc2_bootstrap::flush();
\t\t\t\tPostQuitMessage((arc2_capture_ok && arc2_sequence_ok) ? 0 : 2);
\t\t\t}'''
previous_exit = '''\t\t\ttests.Run();
\t\t\tif (arc2_max_frames && ++arc2_frames >= arc2_max_frames)
\t\t\t{
\t\t\t\tarc2_bootstrap::flush();
\t\t\t\tPostQuitMessage(0);
\t\t\t}'''
current_exit = '''\t\t\ttests.Run();
\t\t\tif (arc2_max_frames && ++arc2_frames >= arc2_max_frames)
\t\t\t{
\t\t\t\tchar arc2_capture[32768]{};
\t\t\t\tGetEnvironmentVariableA("ARC2_WICKED_SCREENSHOT", arc2_capture, sizeof(arc2_capture));
\t\t\t\tconst bool arc2_capture_ok = !arc2_capture[0] || tests.SaveArc2Capture(arc2_capture);
\t\t\t\tarc2_bootstrap::flush();
\t\t\t\tPostQuitMessage(arc2_capture_ok ? 0 : 2);
\t\t\t}'''
if old_exit in main.read_text(encoding="utf-8-sig"):
    patch(main, old_exit, new_exit)
elif previous_exit in main.read_text(encoding="utf-8-sig"):
    patch(main, previous_exit, new_exit)
elif current_exit in main.read_text(encoding="utf-8-sig"):
    patch(main, current_exit, new_exit)
else:
    patch(main, '\t\t\ttests.Run();', new_exit)
