# ARC Mega Stage A — Stages 9–12

Mega Stage A completes the first integrated adaptive-runtime brain in a controlled D3D12 environment. It intentionally keeps temporal assistance out of the acceptance path: native render resolution remains 1920×1080 and DLSS/FSR/XeSS/frame generation/dynamic resolution are off.

## Stage 9 — Runtime Governor Integration

`RuntimeIntegration` now owns a `UnifiedRuntimeGovernor` alongside the existing `LiveRuntimeController`, event bridge and `RuntimeCoordinator`. Hosts can register quality profiles, update importance, query admission and feed frame-budget samples through `tick_adaptive()`.

Quality mutations are not credited using predicted gains. A successful physical mutation is held as a pending effect and the next observed frame-budget sample closes the feedback loop through `ActionEffectTracker`.

## Stage 10 — Resource Admission and Semantic Protection

`QualityAdmissionController` evaluates a resource before the host commits its full physical quality. Inputs include screen coverage, visibility, motion salience, semantic importance, distance, frame pressure and local-memory pressure.

The admission policy is conservative and fail-closed. UI, faces and the player weapon are protected by default. Temporal profiles never participate in native admission. Distant, low-importance resources can be admitted at a lower physical level under pressure while preserving the logical resource contract at the host boundary.

The Mega A hardware benchmark demonstrates this by requesting a 4096×4096 distant texture, asking ARC for an admitted quality level, and physically creating the smaller D3D12 allocation. A parallel UI admission request must remain full quality.

## Stage 11 — Physical Quality Actuation

`RuntimeMutationBackend` now exposes generic `apply_quality()` and `restore_quality()` operations in addition to residency and texture-specific operations. The D3D12 `LiveRuntimeBackend` binds these operations through an explicit host-owned quality mutation callback.

This keeps the boundary safe: ARC decides which already-registered action to take, while the renderer/host owns the physical implementation. Unsupported domains fail closed. No injection, anti-cheat bypass, driver bypass or protected-process manipulation is part of this design.

## Stage 12 — Unified Global Governor

`GlobalActionArbiter` receives an already-safe memory plan and an already-safe quality plan before either is mutated. Its policy is:

- memory emergency receives first priority;
- quality may join an emergency only when memory relief is short and the quality action itself frees physical memory;
- an over-budget frame suppresses normal memory restore/prefetch that would consume headroom;
- moderate simultaneous frame and memory pressure can execute a combined plan;
- normal headroom can restore visual quality and memory residency together when safe.

`RuntimeCoordinator::tick_with_plan()` executes the exact precomputed memory plan selected by this arbitration, so planning is not repeated against changing state.

## Mega A D3D12 acceptance

The benchmark combines real D3D12 geometry, raster/overdraw, texture sampling, lighting and shadow work with controlled physical residency resources and resource admission. It calibrates quality ladders on the local GPU and repeats a dynamic workload schedule as baseline and adaptive passes.

The full acceptance requires all of the following:

- native 1920×1080 and temporal assistance off;
- physical resource admission reduces a distant texture allocation;
- critical/UI semantic protection keeps the protected resource at full quality;
- generic runtime quality mutations execute physically through the backend contract;
- online effect learning observes real before/after samples;
- multiple graphics domains are controlled;
- real D3D12 `Evict -> MakeResident` produces measurable DXGI local-memory relief and restoration;
- the unified memory path executes;
- at least one combined memory+quality arbitration is observed;
- restore path is exercised;
- frame-budget miss ratio or overshoot materially improves;
- all quality ladders and residency state recover at the end;
- no generic quality backend failure occurs.

The acceptance result is published under `results/mega-stage-a/<timestamp>` on a dedicated results branch.
