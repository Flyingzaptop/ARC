"""Add bounded, testbed-only native Present-return timing to pinned source."""
from __future__ import annotations

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / "build" / "arc2-testbeds"
HEADER = (Path(__file__).resolve().parent / "native-present-timer.hpp").as_posix()


def replace_once(path: Path, old: str, new: str) -> None:
    content = path.read_text(encoding="utf-8-sig")
    if new in content:
        print(f"unchanged {path}")
        return
    if content.count(old) != 1:
        raise RuntimeError(f"Expected one Present anchor in {path}: {old!r}")
    path.write_text(content.replace(old, new), encoding="utf-8")
    print(f"patched {path}")


def include(path: Path, anchor: str) -> None:
    replace_once(path, anchor, anchor + f'\n#include "{HEADER}"')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("testbed", choices=("wicked", "cauldron", "miniengine", "diligent", "bgfx"))
    parser.add_argument("--root", type=Path, default=DEFAULT)
    args = parser.parse_args()
    base = args.root.resolve()
    if args.testbed == "miniengine":
        path = base / "DirectX-Graphics-Samples/MiniEngine/Core/Display.cpp"
        include(path, '#include "pch.h"')
        replace_once(path, '    s_SwapChain1->Present(PresentInterval, 0);',
                     '    const auto arc2_present_begin = arc2_native_present::now();\n'
                     '    const HRESULT arc2_present_hr = s_SwapChain1->Present(PresentInterval, 0);\n'
                     '    arc2_native_present::record(arc2_present_begin, arc2_present_hr, 0);')
    elif args.testbed == "wicked":
        path = base / "WickedEngine/WickedEngine/wiGraphicsDevice_DX12.cpp"
        include(path, '#include "wiGraphicsDevice_DX12.h"')
        old = '\t\t\t\t\tHRESULT hr = dx12_check(swapchain_internal->swapChain->Present(swapchain->desc.vsync, presentFlags));'
        replace_once(path, old, '\t\t\t\t\tconst auto arc2_present_begin = arc2_native_present::now();\n'
                     + old + '\n\t\t\t\t\tarc2_native_present::record(arc2_present_begin, hr, presentFlags);')
    elif args.testbed == "diligent":
        path = base / "DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3D12/src/SwapChainD3D12Impl.cpp"
        include(path, '#include "pch.h"')
        old = '    HRESULT hr = PresentInternal(SyncInterval);'
        replace_once(path, old, '    const auto arc2_present_begin = arc2_native_present::now();\n'
                     + old + '\n    arc2_native_present::record(arc2_present_begin, hr, 0);')
    elif args.testbed == "bgfx":
        path = base / "bgfx/src/renderer_d3d12.cpp"
        include(path, '#\tinclude "renderer_d3d12.h"')
        old = '\t\t\tHRESULT hr = m_swapChain->Present(_syncInterval, _flags);'
        replace_once(path, old, '\t\t\tconst auto arc2_present_begin = arc2_native_present::now();\n'
                     + old + '\n\t\t\tarc2_native_present::record(arc2_present_begin, hr, _flags);')
    elif args.testbed == "cauldron":
        path = base / "FidelityFX-1.1.4/framework/cauldron/framework/src/render/dx12/device_dx12.cpp"
        content = path.read_text(encoding="utf-8-sig")
        anchor = next(line for line in content.splitlines() if line.startswith('#include "') and line.endswith('"'))
        include(path, anchor)
        replace_once(path, '        HRESULT hrCode = S_OK;\n        if (pSwapChain->GetImpl()->m_VSyncEnabled)',
                     '        HRESULT hrCode = S_OK;\n        const auto arc2_present_begin = arc2_native_present::now();\n'
                     '        if (pSwapChain->GetImpl()->m_VSyncEnabled)')
        replace_once(path,
                     '            hrCode = pSwapChain->GetImpl()->m_pSwapChain->Present(0, pSwapChain->GetImpl()->m_TearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0);',
                     '            hrCode = pSwapChain->GetImpl()->m_pSwapChain->Present(0, pSwapChain->GetImpl()->m_TearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0);\n'
                     '        arc2_native_present::record(arc2_present_begin, hrCode, 0);')


if __name__ == "__main__":
    main()
