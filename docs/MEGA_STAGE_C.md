# ARC Mega Stage C — Automatic Understanding, Contextual Learning, Safety

Mega C starts from the frozen Stage 14.5 Wicked Engine acceptance:
`freeze/stage14_5-wicked-pass-17af7311`.

It preserves the cooperative/native boundary: no DLL injection, COM vtable
patching, anti-cheat bypass, or protected-process tampering.

## Stage 15 — Automatic Scene Understanding

ARC infers resource and workload semantics only from observed graphics behavior:

- resource dimensions, mip/array shape and allocation kind;
- SRV/UAV/RTV/DSV/CBV evidence;
- read/write balance, recency and reuse cadence;
- draw/indexed-draw/dispatch/indirect density;
- resource-use, descriptor, barrier and copy activity;
- measured GPU frame time.

Truth labels from Wicked Tests are written to the result file only after control
decisions. They are never inputs to the controller.

Resource classes include material texture, shadow map, depth target, color
target, transient target, storage texture, history texture, geometry buffer and
constant buffer. Workload classes include raster, geometry, compute, bandwidth,
shadow, mixed and balanced.

Stage 15 hardware acceptance requires:

- semantic coverage >= 20% at confidence >= 0.55;
- >= 24 confidently classified live resources;
- workload signatures for >= 4/5 truth-test scenes;
- >= 2 distinct inferred workload classes;
- >= 3/5 broad truth-alignment checks;
- no scene labels used by the controller.

## Stage 16 — Contextual Online Calibration

The existing per-action mean-effect model remains the stable prior/fallback.
Mega C adds an online contextual model per action/sequence using:

- GPU busy fraction;
- memory-bandwidth pressure;
- raster pressure;
- geometry pressure;
- lighting/compute pressure;
- shadow pressure;
- local memory pressure.

The model is trained only after a physical action resolves against a later GPU
observation. The action-time context is retained with the pending mutation so
the effect cannot be accidentally attributed to a later scene.

Degrade gain and restore cost remain separate models. Restore planning uses a
conservative contextual estimate.

Stage 16 hardware acceptance requires at least two contextual models and at
least two resolved contextual samples in addition to the existing effect
learning gates.

## Stage 17 — Compatibility and Safety

The compatibility guard is fail-closed.

- malformed/failed telemetry -> ObserveOnly;
- insufficient semantic/resource evidence -> ObserveOnly;
- explicit quality providers may remain QualityOnly;
- External and Reserved resources are never automatically residency-safe;
- residency safety additionally requires a compatible high-confidence semantic
  classification and an existing ResourceGraph safety classification.

The native host can explicitly enable enforcement. When enabled, a Controlled
tick with unsafe compatibility is executed as ObserveOnly for that tick.
Residency opt-in is rejected before backend mutation when the resource is not
semantically safe.

For the pinned Wicked integration, D3D12MA-owned resources are deliberately
observed as External. Therefore the expected Stage 17 verdict is:

`QualityOnly: allow_quality=true, allow_residency=false`.

## Mega C Wicked acceptance

Pinned external renderer:

- repository: `turanszkij/WickedEngine`
- SHA: `0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`
- workload: Wicked `Samples/Tests`
- native client area: 1920x1080 borderless window
- TAA / FSR / FSR2: OFF
- timing: Wicked D3D12 GPU timestamp profiler
- scenes: Model, Shadows, Water, Volumetric, 65k Instances

The previous Stage 14.5 integration/performance gates remain in force. Mega C
adds Stage 15, 16 and 17 gates; they do not replace or weaken those earlier
requirements.

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/mega-stage-c-wicked-bootstrap.ps1
```

Published results use immutable branches named
`results/mega-stage-c-wicked-<timestamp>`.
