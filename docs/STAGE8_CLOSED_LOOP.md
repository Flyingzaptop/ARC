# Stage 8 — Closed-Loop Adaptive Graphics

Stage 8 turns the Stage 6/7 Adaptive Quality Core from a one-shot planner into a live feedback controller.

## Default policy

ARC Core stays native-resolution first:

- render target: 1920x1080 in the benchmark;
- dynamic resolution: OFF;
- DLSS / FSR / XeSS: OFF;
- frame generation: OFF;
- temporal assistance: OFF unless a future user explicitly opts in.

The closed loop may change only reversible native graphics-quality actions in the controlled renderer.

## Controller

`AdaptiveQualityController` adds:

- EWMA filtering of measured frame time;
- overload and headroom hysteresis;
- settle frames after an action;
- minimum hold time before restoring a degradation;
- strict quality-ladder ordering (`0 -> 1 -> 2`, restore `2 -> 1 -> 0`);
- one action per decision by default so that the measured effect can be attributed;
- `ActionEffectTracker` feedback from the observed before/after GPU time;
- counters for degrade, restore, active actions and direction changes;
- first-class Shadow bottleneck classification.

The host renderer still owns the actual graphics objects and applies the returned action. ARC owns the policy.

## GPU acceptance workload

The Stage 8 D3D12 benchmark reuses the hardware-validated Stage 7 renderer:

- native 1920x1080 render target;
- procedural geometry pressure;
- real raster/overdraw layers;
- shader-visible texture sampling;
- iterative lighting work;
- shadow render-target updates and sampling;
- GPU timestamp queries and DXGI local-memory telemetry.

Before the dynamic test, ARC measures a three-step physical quality ladder for each domain:

1. bandwidth / texture sampling;
2. raster overdraw;
3. geometry density;
4. lighting iterations;
5. shadow update/sample quality.

It then calibrates a heavy scene transition for every domain on the local GPU. This avoids hard-coding an RTX 3060-specific load level.

## Repeated schedule

The same schedule runs twice:

1. fixed full quality baseline;
2. ARC closed loop.

Schedule:

`easy -> bandwidth-heavy -> easy -> raster-heavy -> easy -> geometry-heavy -> easy -> lighting-heavy -> easy -> shadow-heavy -> long easy`

The long final easy phase is deliberate: all reversible quality losses must be restored when headroom returns.

## Acceptance semantics

Stage 8 does **not** require aggregate P50 to be lower. Permanently degrading an easy scene would make that metric look good while violating ARC's goal.

The important metrics are:

- frame-budget miss ratio;
- mean overshoot above the target;
- live degrade decisions;
- live restore decisions;
- adaptation across several graphics domains;
- no rapid degrade/restore chatter;
- zero active quality reductions at the end;
- exact return to the full-quality knob state;
- temporal assistance remains unused.

The acceptance runner currently requires a meaningful reduction in either frame-budget misses or overshoot, plus live recovery and anti-chatter gates.

## What Stage 8 proves — and what it does not

A PASS proves that the ARC quality policy can operate as a stable feedback controller over real D3D12 graphics work whose load changes over time.

It does not yet prove transparent control of arbitrary closed-source commercial games. A game or integration layer still needs to expose safe quality/resource actions to ARC. The Stage 8 host/action contract is designed so the same controller can later drive such integrations without changing policy semantics.
