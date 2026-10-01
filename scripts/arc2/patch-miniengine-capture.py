"""Add one opt-in MiniEngine GPU backbuffer readback for image verification."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/arc2-testbeds/DirectX-Graphics-Samples/MiniEngine/Core/Display.cpp"
content = SOURCE.read_text(encoding="utf-8-sig")
anchor = '    const auto arc2_present_begin = arc2_native_present::now();'
replacement = '''    static unsigned arc2_capture_frame = 0;
    if (++arc2_capture_frame == 180) {
        wchar_t arc2_capture_path[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"ARC2_MINI_READBACK", arc2_capture_path, 32768);
        if (n && n < 32768) {
            g_DisplayPlane[g_CurrentBuffer].ExportToFile(arc2_capture_path);
            GraphicsContext& arc2_restore = GraphicsContext::Begin(L"ARC2 testbed restore Present state");
            arc2_restore.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_PRESENT);
            arc2_restore.Finish(true);
        }
    }
    const auto arc2_present_begin = arc2_native_present::now();'''
if replacement not in content:
    if content.count(anchor) != 1:
        raise RuntimeError("MiniEngine Present timing anchor changed")
    SOURCE.write_text(content.replace(anchor, replacement), encoding="utf-8")
print(f"MiniEngine opt-in frame 180 backbuffer readback ready: {SOURCE}")
