"""Opt-in fixed simulation delta for MiniEngine animated glTF image checks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/arc2-testbeds/DirectX-Graphics-Samples/MiniEngine/Core/GameCore.cpp"
content = SOURCE.read_text(encoding="utf-8-sig")
anchor = '        float DeltaTime = Graphics::GetFrameTime();'
replacement = '''        float DeltaTime = Graphics::GetFrameTime();
        if (GetEnvironmentVariableW(L"ARC2_MINI_FIXED_DT", nullptr, 0) > 0)
            DeltaTime = 1.0f / 60.0f; // Testbed simulation only; GPU/QPC timing stays real.'''
if replacement not in content:
    if content.count(anchor) != 1:
        raise RuntimeError("MiniEngine GameCore delta anchor changed")
    SOURCE.write_text(content.replace(anchor, replacement), encoding="utf-8")
print(f"MiniEngine fixed-step glTF route ready: {SOURCE}")
