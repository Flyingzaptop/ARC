# Mega D — GPU attribution, screen contribution and cost

Stages 16 + 17 + 20, observer only. Stage 15's 81/128 complexity result remains
FAIL; the user explicitly accepted deferring that gate to proceed on 2026-09-18.
It is not a prerequisite silently changed to PASS.

## Contract and acceptance

The core consumes explicit ordered WorkObservations, not the old command-wide
presence counters. Recording/reset/close/submit are separate. Re-execution creates
new work IDs. Inputs connect to preceding resource writers; proven full overwrite
removes synchronized previous versions, partial writes retain them. Whole-resource
granularity is conservative: subresource overlap is not yet resolved.

Queue order and observed fence signal/wait establish happens-before. CPU callback
arrival across queues does not. Unknown/future waits invalidate completeness.
Candidate shader bindings are possible accesses, never confirmed shader reads.
Incomplete captures, unknown producers, incomplete binding sets and unsynchronized
dependencies prohibit a complete attribution claim. No optimizer consumes this graph.

Raster contribution is an upper bound in the local output target from target,
viewport and scissor intersection, rounded outwards to pixel boundaries. It is not actual visible coverage or a bound
after arbitrary later composition. Unknown raster state yields null, not zero.
GPU cost is attached only from completed timestamp pairs at the queue's frequency.
Unmeasured work remains null; items and copy bytes remain separate cost proxies.
No claim to cache pressure, bandwidth or per-resource GPU time without measurements.

All capture dimensions have configurable hard limits. Pending recorded work also
shares a global node-sized budget; adding command lists cannot multiply it.
Synchronization clocks are stored once per execution node, not once per resource
access. Overflow fails closed;
clear starts a fresh capture. Host must serialize calls and use stable, unique
resource IDs (including native-pointer lifetime changes). The observer introduces
no quality/resource mutation.
Unmodeled aliasing or missing command events must invalidate the capture; distinct
resource IDs alone do not establish physical-memory independence.

Acceptance requires independently specified small dependency fixtures (including
unrelated branches, overwrite, reset/replay and cross-queue cases), invalid/missing
measurement rejection, bounded storage tests, native GPU timestamp/readback
evidence, and an opt-in pinned Wicked capture. Real renderer partial coverage is
reported as partial, not universal graph reconstruction. Existing Stage 14/15
measurement modes retain their behavior when capture is disabled.

## Implementation plan

1. Implement bounded capture/replay and conservative dependency reconstruction.
2. Validate topology against hand-specified fixtures, raster clipping and timing.
3. Exercise GPU timestamps and data correctness on a native D3D12 workload.
4. Export opt-in renderer work captures; preserve missing bindings/sync as unknown.
5. Build/test, record raw GPU evidence and limitations, commit/publish.

## Pinned Wicked adapter

Set `ARC_WICKED_ATTRIBUTION_OUTPUT` to an absolute JSON path to opt in. The adapter
captures one frame after the Water baseline scene has warmed up and settled, preserving recording order
within command lists and actual submission order. Draw bindings are explicitly a
conservative cumulative candidate set since reset, not an exact root-binding
snapshot. Copies have observed endpoints. The exact presented backbuffer is
captured before Present advances the swapchain index.

The first 64 direct draw/dispatch operations are bracketed with native timestamp
queries. Queries resolve outside render passes before command-list close; ARC
signals diagnostic completion fences after submission and polls without blocking
before reading results. Raw ticks, queue frequency and execution-node ID accompany
every measurement. This diagnostic mode adds GPU query/fence work and is not an
unperturbed performance comparison. It does not drive the quality controller.
These are bracketed queue intervals (including any preparation commands between
the hook and draw), not marginal isolated costs. Intervals on different queues
can overlap; summing them is not frame time. Zero ticks can reflect timer resolution.
Unmeasured work retains null cost. Renderer fence dependencies and bindless shader
access remain incomplete, so a complete Present slice is not claimed.

Core fixtures test exact topology where the host supplies observed complete inputs.
The real renderer exercises the conservative partial-observation path. Raster
bounds are only emitted for a known single viewport/scissor and supported target;
multi-target/multi-viewport cases abstain. The native test verifies three 4 MiB
copies by readback and independently exports all six raw timestamps.

`scripts/validate-mega-d.ps1` independently checks native topology, timestamp
conversion and renderer measurement-to-work association. A PASS is scoped to this
observer prototype, not full shader-level causality or production optimization.

For a prebuilt pinned renderer and native test executable, run
`scripts/run-mega-d-capture.ps1` with `-Executable`, `-WorkingDirectory`,
`-NativeExecutable` and a new `-OutputDirectory`. It records source/binary hashes,
runs the hardware readback test and the observer capture, independently validates
the JSON, and preserves failures. Build both executables from the declared source
first; a binary hash alone is not proof of its source provenance.

## Recorded hardware result

The final pinned capture at source `cf280cf35b499690186ae4ca3ed4431e6410925e`
passed the independent validator. It contains 149 execution nodes, 839
conservative dependency edges, 28 raster-bound observations, 64 measured GPU
timestamp pairs and 24 recomputed Present ancestors. The graph reported zero
capture errors and the native 4 MiB three-copy readback fixture passed.

The capture callback durations summed to 1.8913 ms for this sampled frame. This is
diagnostic observer work across callbacks, not a frame critical-path measurement.
The capture mode is therefore opt-in and samples one settled Water frame; it is
not enabled in normal runs or in performance comparisons.

A separate ARC-OFF control run completed cleanly, but the laptop GPU was under an
active NVIDIA software power cap near 30 W with changing clocks. Its results are
retained as environment evidence, not used to claim a Mega D performance delta.
The existing Stage 14.5/15 acceptance artifacts remain separate and unchanged.

The callback CPU metric sums instrumented observer method durations, including
mutex wait, across threads. It excludes Present export/file I/O and outer host
lookups; it is not a complete frame-critical-path overhead measurement. Capture
defaults OFF, and exactly one settled frame is sampled when explicitly enabled.

The renderer overlay also forwards cooperative semaphore signal/wait callbacks
into the graph. Fence identity uses the native fence lifetime identity and owning
queue; non-monotonic values or reuse by another queue fail closed. Cross-queue
ordering can therefore be represented when the renderer emits these callbacks,
while unknown shader-level bindless use remains explicitly incomplete.
