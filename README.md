# ARC — Adaptive Runtime Core

ARC begins as a read-only D3D12 observer. Its first job is to reconstruct GPU
resource lifetime, memory pressure, queue activity, and use history without
changing application output.

## Current implementation

The integrated observer captures resource and heap lifetimes, views, recorded
commands, submissions, copies/resolves, barriers, fences, presents and DXGI
budgets. A bounded producer ring feeds a background trace collector. Graph
reconstruction, conservative classification and temperature analysis run offline.

Three D3D12 workloads validate GPU buffer, raster, MSAA resolve and compute
outputs against independent expected values. They compare baseline, Light and
Full observation in the same executable. See [Stage 1 status](docs/STAGE1_STATUS.md)
for the exact supported surface and remaining limitations.

## Build

```powershell
./scripts/validate.ps1 -Configuration Release -GpuTests
./scripts/validate.ps1 -Configuration Debug -GpuTests
./scripts/calibrate.ps1
./scripts/benchmark.ps1 -Iterations 10000 -Rounds 3
```

The local script discovers MSVC/CMake/Ninja through Visual Studio Installer and
sets the installed Russian compiler's include prefix. Ordinary CMake builds on
other installations use their compiler's default detected prefix. GPU tests are
opt-in; hosted CI runs the core and D3D12 view-normalization tests.

```powershell
./build/Release/dx12-memory-pressure.exe light 100 0.5
./build/Release/arc-trace-viewer.exe traces/dx12-memory-pressure-light.arcbin build/summary.json build/timeline.csv
```

Sample arguments: `baseline|light|full`, iterations (1–100000), optional memory
pressure fraction (0–0.85 of the current DXGI budget). Default pressure is zero.
The hidden swapchain never takes desktop focus. Output files are under `traces/`.
The viewer also writes `timeline.csv.resources.csv`. The old `arc-trace` executable
remains a small trace round-trip demonstration.

`arc-calibrate [output.json] [file-to-read]` measures CPU/RAM/cache-warmed storage.
`scripts/calibrate.ps1` adds GPU timestamp proxies, CPU identity and OS build to
the versioned hardware profile. Storage inputs are opened read-only.
