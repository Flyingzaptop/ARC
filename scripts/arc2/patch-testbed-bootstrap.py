"""Apply only ARC2 API-creation bootstrap to pinned, ignored testbed trees.

The patch carries no engine semantics or optimization hints. Native mode uses
the original D3D12/DXGI exports; ARC2_MODE selects the generic ARC frontend.
"""
from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TESTBEDS = ROOT / "build" / "arc2-testbeds"
BOOTSTRAP = (ROOT / "include" / "arc" / "arc2" / "bootstrap.hpp").as_posix()
BOOTSTRAP_LOADER = (ROOT / "include" / "arc" / "arc2" / "bootstrap_loader.hpp").as_posix()
MARKER = "// ARC2 testbed bootstrap (creation entry points only)."
UNDEF = "\n".join(
    f"#undef {name}"
    for name in (
        "D3D12CreateDevice",
        "CreateDXGIFactory",
        "CreateDXGIFactory1",
        "CreateDXGIFactory2",
    )
)


def patch(path: Path, anchor: str, replacement: str) -> None:
    if not path.exists():
        raise FileNotFoundError(path)
    content = path.read_text(encoding="utf-8-sig")
    if replacement in content:
        print(f"unchanged {path}")
        return
    if content.count(anchor) != 1:
        raise RuntimeError(f"Expected one bootstrap anchor in {path}: {anchor!r}")
    path.write_text(content.replace(anchor, replacement), encoding="utf-8")
    print(f"patched {path}")


def direct(path: Path, anchor: str) -> None:
    patch(path, anchor, f'{anchor}\n{MARKER}\n#include "{BOOTSTRAP}"')


def dynamic(path: Path, anchor: str, symbol: str, adapter: str, target: str | None = None) -> None:
    patch(path, anchor, f'{anchor}\n{MARKER}\n#include "{BOOTSTRAP}"\n{UNDEF}')
    assignment = {
        "device": "&arc2_bootstrap::device",
        "factory1": "&arc2_bootstrap::factory1",
        "factory2": "&arc2_bootstrap::factory2",
    }[adapter]
    lhs = target or symbol.split(" = ")[0].strip()
    patch(path, symbol, f'{symbol}\n\t\t\tif (arc2_bootstrap::loader().requested) {lhs} = {assignment};')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("testbed", choices=("wicked", "cauldron", "miniengine", "diligent", "bgfx"))
    parser.add_argument("--root", type=Path, default=DEFAULT_TESTBEDS)
    args = parser.parse_args()
    base = args.root.resolve()
    if args.testbed == "miniengine":
        core = base / "DirectX-Graphics-Samples" / "MiniEngine" / "Core"
        for name in ("GraphicsCore.cpp", "Display.cpp"):
            direct(core / name, '#include "pch.h"')
    elif args.testbed == "cauldron":
        sdk = base / "FidelityFX-1.1.4"
        if not (sdk / "framework").exists():
            sdk /= "sdk"
        render = sdk / "framework" / "cauldron" / "framework" / "src" / "render" / "dx12"
        for name in ("device_dx12.cpp", "swapchain_dx12.cpp"):
            path = render / name
            content = path.read_text(encoding="utf-8-sig")
            anchor = next(line for line in content.splitlines() if line.startswith('#include "') and line.endswith('"'))
            direct(path, anchor)
    elif args.testbed == "wicked":
        path = base / "WickedEngine" / "WickedEngine" / "wiGraphicsDevice_DX12.cpp"
        dynamic(path, '#include "wiGraphicsDevice_DX12.h"',
                '\t\tD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)wiGetProcAddress(dx12, "D3D12CreateDevice");', "device")
        patch(path,
              '\t\tCreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY_2)wiGetProcAddress(dxgi, "CreateDXGIFactory2");',
              '\t\tCreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY_2)wiGetProcAddress(dxgi, "CreateDXGIFactory2");\n\t\tif (arc2_bootstrap::loader().requested) CreateDXGIFactory2 = &arc2_bootstrap::factory2;')
    elif args.testbed == "diligent":
        src = base / "DiligentEngine" / "DiligentCore" / "Graphics" / "GraphicsEngineD3D12" / "src"
        direct(src / "EngineFactoryD3D12.cpp", '#include "WinHPostface.h"')
        dynamic(src / "D3D12Loader.cpp", '#include "WinHPostface.h"',
                '    LOAD_D3D12_ENTRY_POINT(D3D12CreateDevice);', "device", "D3D12CreateDevice")
        swapchain = base / "DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3DBase/include/SwapChainD3DBase.hpp"
        patch(swapchain, '#include "D3DErrors.hpp"',
              '#include "D3DErrors.hpp"\n' + f'#include "{BOOTSTRAP_LOADER}"')
        patch(swapchain,
              'CreateDXGIFactory1(__uuidof(pDXGIFactory), reinterpret_cast<void**>(static_cast<IDXGIFactory2**>(&pDXGIFactory)))',
              'arc2_bootstrap::factory1(__uuidof(pDXGIFactory), reinterpret_cast<void**>(static_cast<IDXGIFactory2**>(&pDXGIFactory)))')
    elif args.testbed == "bgfx":
        src = base / "bgfx" / "src"
        dynamic(src / "renderer_d3d12.cpp", '#\tinclude "renderer_d3d12.h"',
                '\t\t\tD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)bx::dlsym(m_d3d12Dll, "D3D12CreateDevice");', "device")
        dynamic(src / "dxgi.cpp", '#include "renderer_d3d.h"',
                '\t\tCreateDXGIFactory = (PFN_CREATE_DXGI_FACTORY)bx::dlsym(m_dxgiDll, "CreateDXGIFactory1");', "factory1")


if __name__ == "__main__":
    main()
