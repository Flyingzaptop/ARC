# Autonomous delivery sequence

User authorization: implement Mega F, Mega G, verify portability on a second
renderer with the same core, then connect to third-party DX12 games. No subagents.
Core policy, evidence formats and critic must not depend on an engine. Engine
names/scene labels are evaluation metadata only. Existing Mega E builds and
milestones are preserved; this work starts at 18d50bc.

## F: independent evidence and reversible transactions (implemented; native validation passed)

- Define portable capabilities, stable target generations, probe identity and
  bounded image/timing evidence. Unknown capabilities cannot authorize mutation.
- Independent image critic receives full/degraded/full captures of a reproducible
  state. Nonmatching state, incomplete readback, temporal drift, invalid pixels
  or unsupported encoding reject acceptance. Pixel safeguards are not proof of
  subjective perceptual equivalence.
- A transaction always restores the reference state before evaluating the trial.
  Keep a modification only after both image and GPU benefit checks pass. Failed
  rollback latches a fault; no further actions are permitted until host recovery.
- Integrate Stage 19 importance as candidate admission/priority, never as the
  critic's verdict. Baseline timing drift and small/no speedups reject acceptance.
- Exercise damaging, harmless, stale, failed and unavailable actions in tests;
  demonstrate physical changes, reference readback and restore on native D3D12.

## G: prediction and bounded safe adaptation (implemented; combined native validation passed)

Use Stage 18 predictions for early restoration. Learn only from validated trial
outcomes, with bounded capacity, cooldown, rejection quarantine, versioned/resettable
state and explicit confidence. Learning must never relax image/safety gates.

## Portability (validated on an independent Microsoft raster renderer)

Run the same core on two independently implemented renderer adapters. Preserve
raw images, timings, actions and rollback results. Compare host code and core hash;
do not claim portability from two differently named instances of one fixture.

## Third-party DX12 connection (read-only prototype exercised in a real game)

Implement a generic D3D12/DXGI observation/interception boundary, enumerate actual
capabilities and validate on an installed suitable target. No engine knobs or
private object semantics may enter the core. Inaccessible/unknown workloads remain
observe-only. Do not claim arbitrary-game optimization based on injection or
successful launch alone. Do not alter system security or bypass protected targets.

Every milestone requires code, local tests, retained negative results and Git
publication. Stage 15's 81/128 gate remains deferred FAIL. Prior GPU power-cap
observations must be checked before interpreting new performance results.

2026-09-19 checkpoint: F/G deterministic and native tests passed, including external
pixel/timestamp/forecast recomputation. Full Release run was 63/64; the unchanged
legacy Mega B smoke failed its lighting-domain requirement with a 0.003 ms first
calibration gain, then passed an isolated rerun with all three domains. Both
results are retained; no threshold was changed and the first FAIL is not hidden.

Final checkpoint: full Release 65/65 and Debug non-GPU 45/45 passed, plus the added
native HRESULT-forwarding test. Independent Microsoft raster validation passed
with the same core. FPV.SkyDive DX12 observation was exercised after manual Steam
sign-in: one initial Present invalid-call remains unresolved; a later 346-second
window passed with zero errors. Real-game optimization is still disabled until
generic live-graph, reference and mutation capabilities are established. See
PERCEPTUAL_DELIVERY_20260919.md for exact scope and retained evidence.
