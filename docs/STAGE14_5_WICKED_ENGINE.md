# Stage 14.5 — Wicked Engine truth test

Stage 14.5 validates ARC against a complex external D3D12 renderer before automatic scene understanding work begins.

## Pinned renderer

- Upstream: `turanszkij/WickedEngine`
- Workload: `Samples/Tests`
- Pinned commit: `0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`
- Renderer backend: D3D12
- Native client resolution: 1920x1080
- Temporal AA / FSR / FSR2: disabled

The hardware bootstrap sparse-checks out `WickedEngine`, `Samples/Tests`, and
`Content` so the actual model, water, shadow, volumetric and 65k-instance scenes
are present. The CI build check omits `Content` because it only validates compile/link.

ARC does not vendor Wicked Engine. The overlay fails closed if the exact upstream
SHA or any expected source anchor differs.

## Integration boundary

Instrumentation is placed at `GraphicsDevice_DX12`, below scene/editor logic.
ARC observes resource lifecycle, bindless descriptor creation, resource use,
legacy transition barriers, copies, draw/dispatch counters, command submission,
completion fences and Present.

Per-draw resource events are not emitted. Resource use is deduplicated per command
list and draw/dispatch counters are aggregated until submission, keeping telemetry
bounded even in the 65k-instance workload.

Resources remain observational/read-only. Stage 14.5 deliberately does not enable
whole-resource residency control; Mega Stage B already validates that path.

## Real renderer quality actuators

The Wicked host explicitly exposes three reversible quality domains:

- Shadow: global 2D/cube shadow resolution ladder.
- Geometry: tessellation enabled/disabled.
- Bandwidth: material sampler mip LOD bias ladder.

ARC owns the decision and online effect learning. Wicked owns the physical mapping
from an ARC quality action to renderer state. Temporal/upscaling assistance remains
disabled throughout baseline, adaptive and recovery phases.

## Workload and controller blindness

Baseline and adaptive phases run the same five scenes in the same order:

1. Model
2. Shadows
3. Water
4. Volumetric
5. 65k Instances

Scene names are truth labels for the result report only. They are never supplied to
the controller.

Before Stage 15 exists, bottleneck hints are derived conservatively from observed
DX12 activity such as draw/dispatch density, resource-use churn, descriptor writes
and barriers. Class-specific pressures are capped so a generally GPU-bound frame
can fall back to `UnknownGpu` rather than being forced into a guessed semantic class.

## GPU timing

Wicked's existing D3D12 timestamp profiler remains the timing authority. The overlay
adds a read-only accessor for the latest unsmoothed `GPU Frame` timestamp result,
captured before Wicked's display averaging.

## Acceptance

The target is calibrated from the baseline GPU-time distribution. A PASS requires:

- exact ARC source SHA and exact pinned Wicked SHA;
- native 1920x1080 with temporal/upscaling disabled;
- Wicked D3D12 GPU timestamp timing;
- no scene labels used by the controller;
- a complex renderer trace with substantial resources, descriptors, submissions and draws;
- at least four valid baseline/adaptive scene pairs;
- clean ResourceGraph and observation/control separation;
- physical quality actions in at least two domains;
- at least two online learned effects;
- baseline miss pressure between 45% and 75%;
- at least 10% miss-ratio reduction and 5% p50 reduction;
- adaptive p99 no worse than 15% above baseline;
- at least two scene-level p50 wins;
- bounded action/direction churn;
- full-quality recovery and zero quality-backend failures.

The thresholds are fixed before the hardware run. They should only be revised when
measurement evidence shows the acceptance definition itself is wrong, not merely to
turn a failing result green.

## Reproducible paths

- Overlay: `scripts/apply-wicked-engine-integration.ps1`
- DX12 patch: `scripts/patch-wicked-dx12.ps1`
- CI compile check: `scripts/stage14_5-wicked-build-check.ps1`
- Hardware bootstrap: `scripts/stage14_5-wicked-bootstrap.ps1`
- Immutable publisher: `scripts/stage14_5-wicked-publish-existing.ps1`

A failed benchmark is publishable and useful for diagnosis. Stage 14.5 is not
formally closed until the pinned hardware acceptance passes.
