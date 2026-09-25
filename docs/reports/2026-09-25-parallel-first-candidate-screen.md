# Parallelism-first CPU candidate screen

Exact-sort development is stopped. This change replaces the old exchange-first
selection in ARC's diagnostic discovery pipeline with automatic cheap topology
screening and explicit economic inputs. It is not yet a continuously running
in-game profitable-offload selector.

## What now runs automatically

`cpu_parallel_screen.py` reads the existing name-blind detector output. Optionally,
it examines bounded inner backedges from saved, image-matched function contexts.
This run screened **43 overlapping intervals** across the previously captured
functions. Overlapping/nested intervals are linked and their costs must not be
summed. No function names, engine pass labels or manual offsets drive the ranking.

It separates these hypotheses:

- Independent fixed-stride maps, including procedural generation without an input array.
- Mask expansion: counts → exclusive integer scan → stable scatter.
- Reductions, with explicit operator/identity/overflow/FP-order requirements.
- Unclosed element loops requiring further dependency analysis.
- Sequential exchange/pointer/control patterns and synchronization: deferred when
  no effective parallel transformation is established.

The screen checks same-stream stride and possible cross-output overlap. It also
rejects the idea that every backward-branch interval is a closed loop: an
unconditional escape lowers feasibility. A reduction-like interval in this corpus
was demoted for exactly that reason. NOP memory-shaped operands are no longer
counted as actual reads.

Observed activity is distinguished from invocation frequency. The first candidate
was sampled at five distinct times across 7.45 seconds; this is evidence of activity,
not a measured calls-per-frame rate or useful CPU duration.

### Economic inputs and selection

Machine-readable records retain unknown values as `null`, not zero: useful CPU
time, batch size, invocation frequency, live-in/out transfer bytes, upload, return,
wait, dispatch and GPU execution cost. Decoded operand sizes are not transfer bytes.
The full path must fit inside useful task time; task savings are not automatically
frame savings.

An optional image-matched `parallel-measurements.json` can supply runtime measurements.
Parent wall time cannot stand in for isolated useful running time. A GPU consumer
requires supporting edge evidence before its residency is used in ranking. Manual
notes remain labelled and cannot silently drive automatic rank. This format is an
input to investigation ranking, not an ownership/equivalence certificate or a new
knowledge-exchange system. The general producer of all these measurements is not
implemented by this patch.

`select-cpu-contracts.py` now uses this ranking. Unsupported sequential/exchange
groups are omitted from capture plans. Without parallel/economic evidence, a
requested deep capture becomes a **bounded entry/return measurement**: at most
64 event slots and 250 ms call budget. The change was checked with an actual
`--mode training` plan: it selected the packing consumer in boundary mode, rather
than selecting sort for another full instruction trace.

## Cheap shortlist

These are investigation candidates, not admitted GPU transformations.

| Candidate / origin | Concurrent work | Remaining serial dependencies | Useful CPU cost | Exchange / waiting | Proposed GPU algorithm |
|---|---|---|---|---|---|
| Record-processing outer loop, `1c9e90–1ca188`; runtime-sampled; rank 1 | Per-record predicates/counts and packing after offsets | Unclosed callees, batch changes, CPU counts/state, floating extrema and publication | **Unknown**. New whole-parent boundary observation: 5.3223 ms, not pure packing time | Full live-in closure unknown; object/material side inputs and CPU metadata consumers must be included | Map → batch-start flags → segmented exclusive scan → stable scatter; only certified reductions |
| Six-index generation, `1d7260–1d72a3`; static child of sampled parent | Independent cells produce separate six-word blocks | Allocation, dimensions, bounds and buffer publication | Unknown; no evidence it runs every frame | Scalar dimensions in, 24 bytes per cell out; result can remain on GPU only with a proved consumer edge | One GPU lane per cell; no sequential emulation |
| Mask expansion, `1ca130–1ca15b`; inner slice of rank 1 | Masks from different records can be counted/expanded independently | Stable prefix offsets, per-batch counts, output-buffer lifetime | Unknown separately; do **not** add it to its parent's cost | Four bytes per emitted bit; CPU return/wait depends on actual consumer | Popcount → exclusive scan → scatter in original record / ascending-bit order |
| Queue-building loop, `1d78d0–1d7a27`; runtime-sampled, deferred | Potential per-record filtering/evaluation | Callee effects, variable output offsets and input aliases unclosed | Unknown separately; parent timing is not enough | Unknown object-input closure; possible producer/packing fusion is only a proposal | Effective equivalent parallel transform not yet closed; lower priority |
| Reduction-like `231a3c–231ab7`; static, demoted | Not established | Unconditional jump leaves the interval; FP recurrence/order semantics | Unknown | Unknown | No supported reduction certificate; recover the real CFG or investigate another candidate |

Manual interpretation, kept separate from automatic selection: prior binary/source
review identified a buffer-size-change guard around index generation. This makes it
a poor recurring-work assumption until frequency is measured. That annotation is
not silently injected into the automated rank.

## Detailed check of automatically selected rank 1

The manual review of the selected interval proposes a **batch**, not one GPU launch
per tiny mask loop:

1. Evaluate each record with original arithmetic and predicates.
2. Identify adjacent compatible batch boundaries.
3. Compute each record's number of output words.
4. Use segmented integer prefix offsets.
5. Scatter outputs in original record order, with ascending bits within each mask.
6. Reduce batch flags/bounds only under a matching numerical contract.
7. Publish the completed buffer while preserving CPU draw metadata/count consumers.

CPU state-changing/draw callees cannot disappear. Floating AABB extrema require
NaN/signed-zero/tie-order handling; floating summation is not silently reassociated.
Keeping the result on GPU requires closing the existing CPU consumers, not merely
labelling the result a GPU buffer.

For the **previously captured workload scenario**, N=65,344 means the record array
alone is 1,045,504 bytes. One output word per record with no skips is 261,376 bytes;
16 mask bits would require up to 4,182,016 bytes. These are scenario calculations,
not the full input closure or measured transfer volume for the new invocation.
Object/material side data, counters and buffer-generation checks are still missing.

### Short native boundary observation

The existing observer automatically targeted the selected parent function
`0x1c9940`. It recorded **two events / one complete call**, not a full trace:

- Entry-to-return wall interval: **5.3223 ms**.
- Measured observer handler time: **0.0971 ms**; explicit event storage: 82,944 bytes.
- Thread CPU-time delta: zero at the available coarse resolution; **not zero useful work**.
- Call frequency, pure packing CPU time and transfer/wait costs remain unknown.

Subtracting handler time does not remove OS exception delivery, waits or unclosed
callee work. Therefore this observation is not entered as `cpu_useful_ms`, and no
deep capture/GPU implementation is economically approved yet.

The first attempt missed the function while the fixture was still loading. It is
saved as a missed observation, not zero frequency. The runner now waits for five
post-load render frames, with a 16-second readiness bound and 8 MiB telemetry bound,
instead of attaching after a fixed four seconds. Both owned fixture processes exited
normally, and the original EXE was restored. No commercial game was run.

## Validation and limits

13 new tests cover feasibility over sample hotness, unknown costs, transfer losses,
positive investigation eligibility, manual notes, unproved GPU residency,
non-recurring/single-element work, cross-output conflicts, FP reduction classification,
NOP operands, unclosed control flow and integer accumulator dependencies. Existing nine discovery tests and five
contract tests also pass. A native entry/return check exercised the resulting plan.

Current outcome is **automatic triage plus a bounded check of its first choice**.
It does not establish a profitable candidate. The next missing measurement is
isolated useful work and invocation frequency for the packed-record batch, alongside
its live-in/out dataflow—not another complete trace and not the entire parent's time.

- [Automatic 43-interval screen](../evidence/parallel-screen-20260925/screen.json)
- [Manual review of the automatically selected candidate](../evidence/parallel-screen-20260925/selected-review.json)
- [Raw boundary observations and selection plans](../evidence/parallel-screen-20260925/boundary-evidence.zip)
- [Archive hashes and restored fixture identity](../evidence/parallel-screen-20260925/archive-manifest.json)

```powershell
python scripts/cpu_parallel_screen.py <probe-directory> <screen.json> --context <saved-logical-plan>
python scripts/select-cpu-contracts.py <probe-directory> <new-plan-directory> --mode training
python tests/cpu_parallel_screen_tests.py
```
