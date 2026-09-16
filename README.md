# ARC — Adaptive Runtime Core

ARC is an experimental adaptive GPU-memory runtime. It started as a read-only D3D12 observer and now includes a **controlled-lab Stage 2 memory governor** that can choose between whole-resource residency changes and incremental tiled-texture mip quality changes.

ARC does **not** currently mutate arbitrary commercial games. Write-side behavior remains restricted to ARC-owned synthetic D3D12 workloads while safety, prediction, arbitration, rollback semantics and physical memory evidence are validated.

## Current architecture

### Stage 1 observer

The observer captures resource and heap lifetimes, views, command usage, submissions, copies/resolves, barriers, fences, presents and DXGI budgets. A bounded producer path feeds asynchronous trace collection; graph reconstruction, conservative classification and temporal analysis run outside the application hot path.

Stage 1.1 added strict multi-producer ordering, capture CPU instrumentation, broader view/format coverage and debug-layer validation. See [Stage 1 status](docs/STAGE1_STATUS.md) and [Stage 1.1 hardening](docs/STAGE1_1_HARDENING.md).

### Stage 2 controlled memory governor

The controlled Stage 2 stack now contains:

- budget-pressure hysteresis and emergency handling;
- fence-safe D3D12 `Evict` / `EnqueueMakeResident` execution;
- confidence-aware reuse prediction with phase-break and stale-prediction handling;
- compulsory vs predictable miss accounting and anti-thrash grace periods;
- a mip-aware texture quality governor for reserved/tiled resources;
- full safe residency and texture candidate surfaces;
- `MemoryArbiter` and `GlobalMemoryPlanner` for cross-policy decisions;
- headroom restoration arbitration;
- multi-pattern memory/latency frontier benchmarking;
- physical DXGI usage evidence and content readback validation.

The combined `dx12-global-memory-lab` forces one memory deficit that cannot be solved by only one action class. ARC must execute both whole-resource eviction and mip demotion, then later execute both make-resident and mip promotion while preserving buffer/texture contents.

See [Stage 2 final controlled-memory acceptance](docs/STAGE2_FINAL.md).

## Final Stage 2 validation

On the target Windows/D3D12 machine, the full acceptance suite is one command:

```bat
run-stage2-final.cmd
```

To also publish the curated results to an isolated timestamped GitHub branch:

```bat
run-stage2-final.cmd -PublishResults
```

A shorter smoke pass is available as `run-stage2-final.cmd -Quick`, but it is not a replacement for the full acceptance run.

The final runner performs Release and Debug validation, stress tests, controlled GPU mutation tests, D3D12 debug-layer validation when Windows Graphics Tools are available, observer overhead measurement, baseline/oracle/autonomous residency benchmarking, a four-pattern five-point memory frontier, and hardware calibration. It produces:

- `traces/stage2-final-acceptance.json`
- `traces/STAGE2_FINAL_ACCEPTANCE.md`
- `traces/benchmark-summary.json`
- `traces/residency-benchmark-summary.json`
- `traces/residency-frontier-summary.json`
- `traces/residency-frontier.csv`
- `traces/tiled-texture-lab.json`
- `traces/global-memory-lab.json`
- `traces/hardware-profile.json`

The result publisher refuses to mix measurements with dirty tracked source code. It creates a separate `results/stage2-final-*` branch and returns to the development branch after pushing.

## Developer validation

Lower-level commands remain available:

```powershell
./scripts/validate.ps1 -Configuration Release -GpuTests
./scripts/validate.ps1 -Configuration Debug -GpuTests
./scripts/validate.ps1 -Configuration Release -GpuTests -DebugLayer
./scripts/calibrate.ps1
./scripts/benchmark.ps1 -Iterations 10000 -Rounds 3
./scripts/residency-benchmark.ps1 -Rounds 6 -Objects 24 -ObjectMiB 2
./scripts/residency-frontier.ps1 -Rounds 2 -Objects 24 -ObjectMiB 2
```

The local validation scripts discover MSVC/CMake/Ninja through Visual Studio Installer. GPU tests are opt-in; hosted CI validates portable core behavior and Windows compilation/tests but cannot substitute for the final physical GPU/DXGI acceptance run on the target machine.

## Trace tooling

```powershell
./build/Release/dx12-memory-pressure.exe light 100 0.5
./build/Release/arc-trace-viewer.exe traces/dx12-memory-pressure-light.arcbin build/summary.json build/timeline.csv
```

Sample observer arguments are `baseline|light|full`, iteration count and optional allocation-pressure fraction. Output files are under `traces/`. The hidden validation swapchain does not take desktop focus.

`arc-calibrate [output.json] [file-to-read]` measures CPU/RAM/cache-warmed storage. `scripts/calibrate.ps1` augments that profile with GPU timestamp proxies, CPU identity, OS build and ARC commit. Storage inputs are opened read-only.
