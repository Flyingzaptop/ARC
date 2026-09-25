# Reusable preparation buffers, existing GPU inputs and mask-only cost

Continues `8cbe54c`. One short fresh/reused pair was run, plus a CPU-only mask
replay. No new broad test matrix, full instruction trace or GPU optimization kernel.
All fixture instrumentation is source-assisted and temporary; it is not a generic
engine-independent data-mapping implementation.

## 1. Allocation is removable, gathering remains expensive

Two 15-second configured runs, same dynamic Instances scene. Warm statistics
exclude the first ten large-batch frames, the final incomplete frame, and the two
audit frames/their completion frames. No pool exhaustion occurred. There were
758 fresh-run and 842 reused-run measured calls, each processing 65,344 records.

| Median per call, ms | Fresh vector | Reused pool |
|---|---:|---:|
| Original loop minus nested flush | 2.7647 | 2.7628 |
| `reserve` / capacity management | 0.0475 | 0.0001 |
| Gather fields and write 48-byte entries | 2.6550 | 2.4023 |
| Input-ready subtotal: reserve + gather | **2.7230** | **2.4024** |
| Explicit release of fresh allocation | 0.1556 | 0 |
| Separate copy of the existing 16-byte records | 0.1315 | **0.1339** |
| Paired headroom before any GPU work | 0.0407 | **0.3474** |

Column medians need not sum exactly. Release happens after preparing/using the
packet and is reported separately. Page first-touch work can fall in gather time;
`reserve` timing alone is not every consequence of allocating fresh memory.

The pool has four nonblocking leased slots with bounded capacity. A separate
reusable buffer measures the original-record copy **after** gathering, so that
copy does not pre-touch the destination and make fresh gathering appear cheaper.
The copy result is consumed, and an indirect volatile memcpy call prevents dead
copy elimination. Logging/evidence I/O are outside these intervals.

**Reusing memory helps**, including allocator/free and first-touch effects. It
does not remove the repeated object-field reads: they still take about 2.40 ms,
roughly 87% of the original loop's elapsed time. The old rejection of the allocating
implementation is not treated as a proof that this version must lose. This version
has a small positive CPU-side budget; full GPU-path profitability remains unknown.
Rather than repeat the 48-byte gather, the promising next boundary uses existing
GPU fields and transfers only records/missing data.

## 2. Existing records can be reused directly

The original sorted records already form a contiguous 16-byte array. No conversion
is necessary to carry mesh/instance identity, half distance, camera mask and LOD
override. Copying 1,045,504 bytes into an already allocated CPU buffer measured
0.1339 ms. This is a CPU copy measurement, **not a measured GPU upload**.

The source array's lifetime still must cover a real upload. A borrowed CPU pointer
is not automatically GPU-accessible or safe for deferred use. No live lifetime
substitution was introduced here.

## 3. GPU field correspondence and freshness were checked

Added a read-only, two-frame audit immediately after the fixture's existing upload
to `Scene::instanceBuffer`. It copies that actual DEFAULT buffer into a private
readback resource, restores its expected state, then reads it only on a later
frame after `WaitForGPU` returns. CPU reference fields were captured at the upload
point. The source resource is held through completion.

Independent replay of the raw snapshots verified:

- 65,536 objects per capture; actual GPU stride 256 bytes.
- Center, fade distance, radius and stencil match their CPU references exactly.
- The current scene's alpha representation and alpha-test predicate also match.
- First capture: frame 380, checked after frame 381.
- Second capture: frame 381, checked after frame 382.
- **65,535 centers changed** between captures. Comparing the second GPU snapshot
  against the previous CPU snapshot rejects those stale values; comparing it
  against the current reference produces zero mismatches.

This establishes correspondence and freshness **at the audited point in this
controlled renderer**. An optimization must use the same producer dependency, or
an equivalent explicit queue/fence dependency. It does not authorize arbitrary
queues, changed layouts, resized resources or reuse of stale snapshots. The two
readbacks/waits are diagnostics, not proposed per-frame production costs.

| Input | Already available | Remaining condition |
|---|---|---|
| Original record fields | Contiguous CPU array | Upload/lifetime; no repacking needed |
| Fade distance, radius, center | Existing GPU instance buffer, exact in audit | Correct instance mapping, current producer generation and dependency |
| Stencil | GPU flags, exact in audit | Same mapping/flags encoding |
| Alpha-test input | Packed half of `1 - alphaRef` | Preserve the required predicate, not assume recovered full float is identical |
| Color alpha / transparency | Half-precision color alpha | **Not generally enough for original full-precision decisions** |
| CPU object LOD when override is `0xff` | No direct raw LOD field | Need extra value or a separately proved invertible mapping |

A concrete transparency counterexample is included in the analysis: float32 alpha
approximately 0.9999 rounds to half 1.0. Original transparency is positive, but
transparency derived from the half value is zero. That can change the alpha-test
flag even if the packed dither nibble is unchanged. Matching opaque values in these
two frames is not permission to ignore this information loss.

Geometry offsets/counts are not a general substitute for raw CPU LOD. The producer
uses `GetLODSubsetRange`, which clamps LOD and ignores it for meshes without LOD
subsets. Distinct CPU LOD values can therefore have the same GPU geometry range.

### Concrete direction without a second gather

For a controlled next experiment, upload the existing record array directly and
reuse the audited GPU instance fields. If exact transparency/raw LOD cannot be
proved derivable, emit an ARC-owned sidecar while the **existing producer already
has those values**, rather than walking the object array again. A conservative
sidecar could use 8 bytes per object for full transparency and LOD.

That would be about 0.5 MiB per frame for 65,536 objects, shared by both passes,
plus the original record uploads. It is a proposed contract, **not an implemented
or measured saving**. Producer instrumentation, numerical behavior, buffer lifetime,
remaining CPU group consumers and the complete path still need validation.

## 4. Masks measured separately, at whole-batch scale

`arc-mask-cost` replays the mask-only source operation on a captured 65,536-record
packet. Dither/base values are prepared before timing, representing live-ins that
the original outer work must still produce. The baseline writes the original
packed words to a mapped D3D12 UPLOAD buffer. The alternative prepares one batch
of 16-byte tasks, including CPU prefix offsets/counts, in another reused UPLOAD
buffer. Both flush writes before the ready timestamp; neither allocates during
timing. The output exactly matches the captured original words.

| Median over 20 measured interleaved repetitions | Time |
|---|---:|
| Mask expansion itself | **0.05730 ms** |
| Whole-batch input preparation | **0.05595 ms** |
| Difference before GPU transfer/dispatch/computation | **0.00135 ms** |

Ranges overlap: original 0.0568–0.0786 ms, preparation 0.0543–0.0626 ms. The
1.35 µs difference is not a demonstrated replacement advantage. Prepared input is
1 MiB versus 256 KiB of original output for this one-bit-mask workload.

This is an **isolated source-equivalent replay**, not an exact binary timing of
the live inner loop. It intentionally assumes live-ins are already available;
materializing additional live-in arrays in the game would add work. No parent
2.76/5.32 ms timing is assigned to masks. No GPU dispatch was launched, per mask
or per batch, and no end-to-end GPU time is fabricated.

The standalone host-prepared mask route has no meaningful demonstrated budget.
It remains useful only as a prospective stage in the wider chain, where input
and GPU-consumer work are already shared. Different mask distributions or resident
inputs are different contexts; the result is not generalized to every mask workload.

## 5. Execution-condition invalidation

`context_key` now includes `execution_conditions`. The control feedback producer
derives the ocean condition from observations and attaches an inactive-work rejection
only when it is uniformly false. A true or mixed observation cannot retain that
false-state rejection. The producer also no longer forces a move to the grid if an
improved packet route remains the current research candidate.

A regression test exercises the actual observation-to-context conversion:
false → reject; false again → keep rejection; true → invalidate the old record,
clear stale frequency/cost facts and permit bounded research. Continuous operation
still requires its observation provider to publish the current condition; this
does not create a generic game-condition watcher or infer state from absent calls.

## Artifacts and scope

- [Independent analysis](../evidence/packet-reuse-20260925/analysis.json)
- [Whole-batch mask timings](../evidence/packet-reuse-20260925/mask-cost.csv)
- [Raw CPU/GPU snapshots, controls, patches and binaries](../evidence/packet-reuse-20260925/raw-reuse-audit.zip)
- [Archive hashes](../evidence/packet-reuse-20260925/archive-manifest.json)
- [Reopened pooled-route research, without a profitability claim](../evidence/packet-reuse-20260925/routing.json)
- Renderer, bridge source and original EXE were restored byte-for-byte.
- 20 selector tests pass, including condition reactivation. No historical matrix
  or full instruction trace was repeated.

The useful new result is **measured buffer-reuse savings plus verified availability
of current GPU fields**. No game acceleration or live automatic substitution is
claimed. The next useful experiment is the wider shared-input path, not a separate
tiny mask offload.

Replay:

```powershell
python experiments/record-pack/analyze_reuse.py <archive>/raw docs/evidence/packet-reuse-20260925/mask-cost.csv <analysis.json>
cmake --build build --config Release --target arc-mask-cost -j 2
build/Release/arc-mask-cost.exe <archive>/raw/reuse/calls.csv.input.bin <archive>/raw/reuse/calls.csv.output.bin <mask-cost.csv>
python tests/cpu_parallel_screen_tests.py
```
