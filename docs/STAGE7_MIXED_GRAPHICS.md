# Stage 7: Mixed Graphics Benchmark

Stage 7 validates the Adaptive Quality Core against a repeatable ARC-owned D3D12 graphics workload rather than a compute-only proxy.

## Fixed baseline

- Native render target: 1920x1080.
- No dynamic resolution.
- Temporal assistance, DLSS, FSR, XeSS and frame generation are disabled.
- The benchmark runs on the high-performance D3D12 adapter selected by DXGI.

## Physical graphics domains

Each benchmark frame contains real GPU work in several domains:

1. **Geometry** — a procedural high-count triangle pass. The geometry quality knob changes submitted primitive density.
2. **Raster** — repeated native-resolution fullscreen raster layers. The raster quality knob changes overdraw/work submitted at full resolution.
3. **Texture / bandwidth** — the main pixel pass performs repeated shader-visible texture sampling.
4. **Lighting** — the main pixel pass performs a configurable per-pixel light loop.
5. **Shadows** — a separate shadow render target is updated, transitioned to shader-resource state, sampled by the main pass, then transitioned back for the next frame.

The workload deliberately keeps temporal methods outside the native quality optimizer.

## Measured action calibration

Stage 7 does not rely solely on hand-written expected performance gains. After the baseline phase, the benchmark applies each candidate quality step independently for a short micro-probe and measures its physical GPU P50 effect using D3D12 timestamp queries.

Measured gains are then passed to the same `AdaptiveQualityOptimizer` used by the core. The adaptive phase therefore chooses actions using GPU-local evidence instead of benchmark-specific fixed scores.

## Acceptance

The publishing script records PASS/FAIL plus the raw benchmark JSON. Acceptance requires:

- valid mixed-graphics output;
- native 1920x1080 target;
- temporal assistance unused;
- all quality-domain probes present, with measurable gains in at least two domains;
- at least one adaptive action selected;
- actions spanning at least two quality domains;
- physical P50 GPU time improvement of at least 3%;
- P99 GPU time not regressing by more than 0.15 ms.

These gates validate the ARC-owned mixed graphics path. They do not imply the same percentage gain in arbitrary commercial games.

## Benchmark Center

`arc-stage7-benchmark-ui.exe` launches the 60-second mixed benchmark and watches `results/stage7-local/last-summary.txt`. The final PASS/FAIL, P50/P99 deltas, action/domain counts and published GitHub URL remain visible in the UI. `Copy Results URL` copies the published branch URL.

When launched from the UI, the PowerShell result window is kept open until the user presses Enter.
