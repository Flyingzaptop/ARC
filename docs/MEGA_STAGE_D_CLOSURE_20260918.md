# Mega D closure — Stages 16 + 17 + 20

**Status:** CLOSED as the scoped observer milestone.  
**Date:** 2026-09-18  
**Branch:** `codex_den/mega-d-attribution`

Mega D is intentionally closed at the observer/attribution boundary. It does not
claim visual importance, perceptual equivalence, quality actuation, complete
bindless shader tracing, or product performance gains. Those belong to later
MegaStages.

Stage 15's measured-workload complexity result remains **81/128 FAIL** and is
still deferred technical debt. Mega D closure does not rewrite that result.

## Definition of Done

Mega D consists of Stages 16, 17 and 20.

### Stage 16 — GPU dependencies

Required:

- explicit work nodes for ordered GPU work rather than command-wide presence only;
- resource producer/consumer dependencies;
- command recording separated from execution;
- reset/replay creates correct execution identities;
- full versus partial overwrite behavior is conservative;
- cross-queue order is never inferred merely from CPU callback order;
- known fence signal/wait order can establish happens-before;
- uncertain candidate shader bindings remain uncertain;
- incomplete evidence makes attribution incomplete instead of fabricating certainty;
- capture storage is bounded and overflow fails closed;
- a Present slice can be reconstructed from the graph.

**Result:** satisfied by `GpuAttributionGraph` and its deterministic fixtures.
The real Wicked adapter is deliberately partial: cumulative candidate bindings
are not claimed as actual bindless shader reads, and renderer-side observation
does not claim a universally complete Present slice.

### Stage 17 — screen-space contribution

Required:

- derive a backend-neutral screen/raster contribution signal from observed D3D12
  state without engine camera/object metadata;
- unknown raster state must remain unknown rather than become zero;
- bounds must be conservative at target/viewport/scissor boundaries;
- independent validation must recompute the exported result from raw raster state.

**Result:** satisfied for the first Stage 17 milestone by
`raster_coverage_upper()`. It is a **local render-target upper bound**, rounded
outwards to pixel boundaries. It is not actual visibility, occlusion, final-screen
coverage after arbitrary composition, or visual importance.

Those stronger concepts belong to Stages 18 and 19.

### Stage 20 — cost attribution

Required:

- attach cost only from measured GPU evidence;
- retain raw timestamp ticks and queue frequency;
- independently recompute timestamp duration;
- associate each measurement with a concrete work node;
- leave unmeasured work unknown;
- do not pretend work-item counts or copy bytes are measured GPU time.

**Result:** satisfied by native D3D12 timestamp/readback validation and sparse
Wicked per-work timestamp capture. The diagnostic intervals are not additive frame
time and are not claimed as marginal isolated shader cost.

## Validation already present

Portable/deterministic validation covers, among other cases:

- known producer chain and unrelated branch exclusion;
- recording order versus submission order;
- cross-queue unsynchronized dependencies;
- known fence synchronization;
- missing/invalid synchronization rejection;
- partial and full overwrite behavior;
- read-modify-write behavior;
- command replay/reset and retirement;
- possible versus observed access evidence;
- raster clipping and fractional-pixel conservative rounding;
- invalid timestamp rejection and timestamp conversion;
- bounded node/edge/resource/queue/command storage;
- high-churn command lifetime behavior.

The native D3D12 fixture additionally validates a real three-step 4 MiB copy
chain, GPU readback correctness, dependency endpoints, six raw timestamp values
and per-node timing conversion.

The Wicked path captures a settled frame, exports work/resource edges, raster
bounds, sparse timing evidence and the concrete presented backbuffer. The
independent validator recomputes raster bounds, timestamp conversion and Present
ancestry and rejects tampered evidence.

## Hardware status at closure

The previous local agent reported that the final Mega D functional GPU validation
passed again.

During the later performance-control work, the NVIDIA laptop GPU entered an active
**software power-cap** state around **30 W**, with changing reduced clocks. The same
slow state reproduced with **ARC OFF**; the reported Water control was about
**11.1 ms** while the software power cap remained active.

Therefore:

- the functional Mega D GPU validation is retained as correctness evidence;
- the affected runs are **not** used to claim clean ARC overhead or a performance
  delta;
- Mega D does **not** claim a speedup;
- a clean end-to-end performance characterization can be repeated later on a
  stable power state, but it is not a Stage 16/17/20 acceptance requirement.

This distinction matters because Mega D is still observer-only. Its purpose is to
prove that ARC can construct useful, bounded, uncertainty-aware attribution,
screen-region and measured-cost evidence. Perceptual optimization starts later.

## Explicit limitations carried forward

The following are known boundaries, not hidden failures:

1. **Bindless shader access is incomplete.** Candidate descriptors/resources are
   possible accesses, not proof of dynamic shader indexing.
2. **Real-renderer Present attribution can remain incomplete.** Missing bindings,
   unknown producers or synchronization keep the slice incomplete.
3. **Resource granularity is conservative.** Subresource/aliasing precision is
   not yet a universal solved problem.
4. **Raster coverage is only a local upper bound.** It does not establish
   visibility or perceptual importance.
5. **GPU timing is sparse.** Unmeasured work has no fabricated `gpu_ms`.
6. **No optimizer consumes Mega D output yet.** Mega D remains observer-only.
7. **Stage 15 81/128 complexity debt remains open.**

None of these should be silently upgraded to stronger claims in later docs.

## Immutable result workflow

New captures should use:

`scripts/run-mega-d-capture.ps1`

A completed capture contains at minimum:

- `native.json`
- `attribution.json`
- `renderer.json`
- `manifest.json`
- `acceptance.json`

To preserve a capture as immutable GitHub evidence:

`scripts/publish-mega-d-existing.ps1`

The publisher creates:

`results/mega-d-<timestamp>`

with artifacts under:

`results/mega-d/<timestamp>/`

Result branches are archival evidence and must not be merged into development
branches.

## Transition to Mega E

Do not extend Mega D merely to make the graph look more complete.

The next product question is no longer:

> What GPU work is connected to this frame, where can it write, and how much does
> measured work cost?

Mega D now provides that first conservative answer.

Mega E (Stages 18 + 19) should answer:

> What contribution is actually visible now, how is that visibility changing over
> time, and how visually important is the work to the player?

That is the next milestone.
