# ARC Stage 2 — Final Controlled-Memory Acceptance

Stage 2 closes the controlled-memory research loop that started after the read-only Stage 1 observer. ARC still does **not** mutate arbitrary commercial games. All write-side behavior remains restricted to ARC-owned D3D12 laboratory resources.

## What Stage 2 now contains

### Residency governor

- budget-pressure hysteresis with Normal / Pressure / Emergency states;
- strict safety classes (`ControlledSafe`, `Pinned`, `Unknown`);
- fence-safe eviction eligibility;
- reuse prediction with confidence, deviation, stale-prediction decay and phase-break handling;
- post-miss anti-thrash grace periods;
- compulsory vs predictable miss accounting;
- speculative prefetch only in safe headroom;
- deterministic candidate surfaces for higher-level arbitration.

### Texture-quality governor

- per-texture mip-safe classification;
- ordered demotion and promotion steps;
- explicit bytes-freed and quality-loss accounting;
- cooldown / minimum-change age;
- pinned and unknown resources remain immutable;
- complete safe marginal candidate surfaces for global arbitration.

### Global memory planner

`GlobalMemoryPlanner` combines both specialized governors without bypassing their safety rules. Under pressure it compares:

- whole-resource eviction;
- incremental texture mip demotion.

The common objective is a weighted penalty per byte:

`quality_loss + latency_risk + uncertainty`, normalized by bytes freed.

When headroom returns, the same layer arbitrates between:

- make-resident / prefetch;
- texture mip promotion.

The restore planner has a hard headroom cap and preserves per-resource dependency ordering.

### D3D12 physical labs

`dx12-residency-lab`

- baseline / oracle / autonomous modes;
- mixed, cyclic, phase-shift and streaming access patterns;
- configurable managed working-set fraction;
- content verification after eviction / make-resident;
- P50/P95/P99 and miss telemetry.

`dx12-tiled-texture-lab`

- reserved tiled texture;
- physical top-mip unmapping and heap release;
- measured DXGI usage change;
- lower-mip preservation;
- top-mip recreation and re-upload;
- readback verification.

`dx12-global-memory-lab`

- a 64 MiB pageable D3D12 buffer and a reserved tiled texture coexist;
- one synthetic pressure target is handed to `GlobalMemoryPlanner`;
- the planner must choose both whole-resource and mip-level actions;
- chosen actions are executed on the real D3D12 objects;
- DXGI local-memory usage is sampled before / after relief;
- headroom is then exposed and the restore arbiter must execute both make-resident and mip-promotion classes;
- buffer and texture contents are verified after restoration;
- D3D12 debug-layer ERROR/CORRUPTION messages are a hard failure when Graphics Tools are available.

## Final one-command validation

From the repository root on the target Windows machine:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stage2-final-validate.ps1
```

A shorter smoke pass is available with `-Quick`, but it is not a replacement for the full acceptance run.

The full runner performs:

1. Release build, GPU tests and stress tests;
2. Debug build and GPU tests;
3. separate D3D12 debug-layer probe and full debug-layer validation when available;
4. rotated Baseline / Light / Full observer overhead benchmark;
5. rotated baseline / oracle / autonomous residency benchmark;
6. a four-pattern, five-memory-point residency frontier;
7. CPU / RAM / storage / GPU calibration;
8. one machine-readable and one human-readable final verdict.

Primary outputs:

- `traces/stage2-final-acceptance.json`
- `traces/STAGE2_FINAL_ACCEPTANCE.md`
- `traces/benchmark-summary.json`
- `traces/residency-benchmark-summary.json`
- `traces/residency-frontier-summary.json`
- `traces/residency-frontier.csv`
- `traces/tiled-texture-lab.json`
- `traces/global-memory-lab.json`
- `traces/hardware-profile.json`

## Verdict meanings

- `ACCEPTED`: controlled correctness, debug-layer validation, frontier guard and observer performance targets all pass.
- `ACCEPTED_WITH_EXTERNAL_VALIDATION_PENDING`: controlled correctness passes, but Windows Graphics Tools / D3D12 debug layer are unavailable on the machine.
- `ACCEPTED_WITH_PERFORMANCE_WARNINGS`: correctness and debug-layer validation pass, but the conservative frontier or observer performance targets are not all met.
- `NOT_ACCEPTED`: a controlled correctness invariant failed. The script exits non-zero.

The observer Light-mode targets remain deliberately strict: <= 0.15 ms P99 delta and <= 2% total CPU delta versus the unobserved baseline. The frontier requires at least one managed-memory point whose worst P99 increase is <= 0.15 ms and whose aggregate predictable misses remain <= 1 per 1000 measured epochs.

## Result publication

Validation artifacts can be published without mixing them into the development branch:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\publish-stage2-results.ps1
```

The publisher refuses to run when tracked source files are dirty, creates a timestamped `results/stage2-final-*` branch, copies only the curated acceptance artifacts, commits them, pushes the branch, returns to the source branch and prints the GitHub results URL.

## Boundary after Stage 2

A successful Stage 2 acceptance means ARC has a tested controlled-memory decision loop: observe -> predict -> classify -> arbitrate -> mutate controlled resources -> validate -> restore.

It does **not** yet authorize arbitrary-game resource mutation. The next engineering stage should integrate the global planner with a real adapter/resource interception path while preserving Unknown/Red no-touch rules, rollback, observer-only fallback and per-title opt-in validation.
