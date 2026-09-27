# Composite CPU loops: generated GPU routes, negative complete economics

## Result

Continuation of `07cf063`. Both measured large-loop candidates were investigated,
not just the first negative route. Restricted composite models were recovered
from machine instructions and bounded observations, then lowered to GPU kernels.
The generated routes reproduce the required output and metadata on isolated real
inputs. Neither route is economical with the current CPU input gather. No live
replacement was enabled and no application CPU work was removed. Full-frame or
FPS improvement is **not measured or claimed** in this stage.

| Isolated operation | Elements | Original CPU median / mean, ms | Steady input gather, ms | GPU route median / mean, ms | Composed typical / mean cost, ms |
|---|---:|---:|---:|---:|---:|
| Filter and no-growth append | 65,344 | 2.0819 / 3.3436 | 3.2715 | 1.4612 / 1.4999 | 4.7327 / 4.8041 |
| Initialized homogeneous group, word packing | 65,343 | 2.4539 / 2.7105 | 6.8016 | 1.5082 / 1.5782 | 8.3098 / 8.3013 |

“Composed” adds separately measured nonoverlapping input gathering and the complete
GPU tail. It is not a same-process game measurement. Typical cost adds median
gather to median tail; mean cost adds mean gather to mean tail. Three recorded
gather runs have a different sampling scope from the 20 timed GPU repetitions. This is enough
to reject these routes, not to estimate a precise change in game FPS.

CPU warm samples are preserved, including outliers: append
`[8.4059, 2.1366, 2.0790, 2.0144, 2.0819]` ms; word packing
`[2.3053, 3.9870, 2.3460, 2.4539, 2.4603]` ms. No slow sample was discarded.
CPU timing executes the corresponding original instruction span in an isolated
process with entry/exit thunks. It is wall time, including memory stalls and
scheduling, not a sum of worker-thread times. Compilation, process setup and
snapshot restoration are outside this operation timer.

## Defects fixed first

* `POP [RSP]` capture accounts for the destination address being evaluated after
  the stack increment. Unsupported address-size forms are rejected. The typed
  interpreter follows the same ordering, including `POP RSP`; targeted tests
  cover both destinations.
* HLSL generation hashes the exact bytes written. Two saved local contracts and
  two corresponding entries in the previous evidence archive were corrected.
  All 294 entries of that archive were verified; 292 unrelated payloads remain
  byte-identical. No old game trace or shader was regenerated to repair hashes.
* An actual GPU comparison exposed an FP16 rounding mismatch (11 of 32 records,
  one ULP). An explicit binary32-to-binary16 round-to-nearest-even lowering
  replaced the inappropriate conversion. The failing evidence is retained;
  the corrected independent sample and complete batch pass exact comparison.

## What was inferred, and what was supplied by the investigator

The two candidates came from the existing machine-code survey. Selection between
them, experiment sequencing and implementation of supported IR/lowering rules
were performed by the agent. This is not a learned universal task extractor.
No engine source, function name, semantic field name or manually implemented
Wicked algorithm supplies the expression executed by the generated GPU kernels.

Scripts reconstruct register and memory dependencies, predicates, stores,
iteration increments and carried values from instruction bytes and observations.
The no-growth append shape is recognized from its machine-code callee. Pointer
load targets supply otherwise missing snapshot ranges. GPU inputs are bounded
recipes for initial registers and memory leaves, rather than captured CPU output
records. Packet loops evaluate these recipes on independently captured inputs.

The GPU backend uses reusable local-prefix/group-prefix/scatter primitives.
These are general algorithms, not recovered engine logic. The first route writes
16-byte records in original order and produces count, cursor and validity
metadata. The second writes four-byte words in original order and produces nine
metadata words, including count, final index, counter, accumulated flag,
invalid count and the required last-element state.

Carried cursor/index values are not independently supplied for each element:
they are reconstructed from the initial state and GPU prefix. The word flag is
a proven binary OR recurrence. Conditional moves retain their actual condition,
not just the outcome seen in one trace. Unknown guards invalidate publication;
they are never interpreted as an ordinary filtered-out item.

## Supported classes and evidence

These are isolated model/replay classes, not complete live admission contracts.
Non-induction live-ins agree at the sampled path boundaries, but invariance over
all untraced branches is not established. A successful full-batch byte comparison
validates that particular captured input; it does not prove every future input.
The generated contracts therefore keep `live_binding_allowed=false`.

### Filter and append

The admitted snapshot class has a bounded loop, the reconstructed observed
predicate path and a no-growth output allocation with room for the worst-case
batch. Unsupported predicates, calls, growth or memory leaves fail admission.
Reject-path observations are retained, but the successful full-batch case is
the supported positive path; this does not establish arbitrary mixed game
branches. Independent per-path observations and the natural 65,344-item loop
were compared against captured output. Generic mixed prefix/scatter primitives
were also checked separately, including invalid rows and a partial final group.

Original-code replay exactly matches all records, cursor/header and the exit
register/vector context. GPU output matches the complete records and metadata.
The source output protection is ordinary writable memory (`0x4`), so a false
“writecombine preserved” field is not a failure for this candidate.

### Word packing

The admitted suffix starts after the original CPU has initialized the group.
It contains 65,343 iterations of the supported homogeneous group and one-bit
mask path. The initializing first item is not included in the claimed replaced
span. New groups, growth, calls, unknown masks or unsupported transitions stay
outside the contract. These restrictions are checked, not assumed universal.

Original-code replay matches every output word, five stack-state ranges and
the full exit context. GPU output and all nine metadata words match. The
original output is write-combined (`0x404`); isolated replay preserves and checks
that protection before timing. Last-item metadata is extracted once by bounded
input recipes, not copied from a precomputed CPU result for every item.

Both original-code replays audit writes in their own isolated mappings. Warm
replay is allowed only after the audit finds no writes outside declared outputs,
stack and intentional private code normalization. Differences in the source
process's whole before/after VM images are not treated as proof of task writes:
other threads may have changed them. Source-process snapshots do not establish
exclusive ownership or a globally coherent live snapshot.

## Economics and timing boundaries

The first native gather was reduced from 9.2728 to 3.6349 ms by hoisting repeated
work. Separating diagnostic range statistics gives 3.1998 ms steady gather plus
3.7855 ms diagnostic verification (6.9853 ms together). Range/bounds/alias guards
remain in the steady path. The second gather costs 6.6403 ms plus 8.5192 ms of
separate diagnostic verification (15.1595 ms together). Even the favorable costs
excluding these diagnostics lose to the original CPU spans.

The table uses the final evidence-preservation repeats, not the best prior
single observation: append gather `[3.2715, 3.2113, 3.4299]` ms and word gather
`[6.5574, 6.8101, 6.8016]` ms. Their packed-input SHA256 values match the GPU-tested
inputs. Raw stdout and a cost-analysis script reproduce the reported statistics.

The GPU tail includes packed-input preparation/copy, command recording,
submission, upload, generated evaluation, local and group prefix, scatter,
required readback, wait and CPU consume copy. Intermediate scratch stays on GPU;
there is no intermediate CPU round trip. Byte comparison is performed separately
from the measured tail. Compilation and device creation are outside that timer.

The first input packet is 8,102,656 bytes, returning 1,045,504 bytes plus 16 bytes
metadata. The second input is 8,886,648 bytes, returning 261,372 bytes plus 36 bytes
metadata. Median evaluation itself is about 0.0394 and 0.0386 ms, respectively.
Median upload GPU timestamps are about 0.6517 and 0.7133 ms. Those GPU timestamps
are already contained in wait/full tail time and are **not added again**.

The remaining CPU gather alone exceeds the corresponding median original work
for both candidates. Small original CPU sample counts and scheduling outliers
limit precision, but do not support enabling either current route. This rejects
these concrete implementations, not the possibility of profitable GPU offload.

## Exact remaining mechanism

A promising continuation must eliminate reconstruction of a large packed input
on CPU: bind the recovered input expressions directly to proven-fresh existing
GPU buffers, or expand the producer/consumer chain so the necessary layout is
produced and retained on GPU. It must establish the identity, layout, generation
and readiness of those buffers; matching field bytes alone is insufficient.
Then remeasure the entire route before investing in live replacement.

If such a route becomes economical, live integration still needs a real source
for exclusive access/lifetime guarantees, versioned inputs, consumer-compatible
publication, fences and fallback before original work is suppressed. Those
guarantees are not supplied by a snapshot replay. They are separate from the
demonstrated economic rejection and were not replaced with unsupported flags.

## Evidence and reproducibility

Sources, generated HLSL, contracts, raw GPU CSVs and raw original-code measurements
are in `experiments/cpu-composite-gpu`, `experiments/cpu-word-gpu` and
`experiments/cpu-composite-native`. The new evidence archive and inventory are
under `docs/evidence/cpu-composite-20260927`; the previous archive's explicit
hash-correction record is under `docs/evidence/cpu-chain-20260927`.

Published bulk VM images retain only exact provenance inputs and declared
outputs; unrelated bytes are zeroed while addresses and sizes remain unchanged.
Original and redacted SHA256 hashes are recorded. Both redacted snapshots still
pass complete original-code replay. The originals were not modified. Evidence
replay is restricted to owned isolated processes, never the original application.

The stand was used only for missing bounded observations. A stale-DLL capture
after a failed build was marked invalid and excluded. The runner now refuses a
capture DLL older than its sources. No commercial game was started.

## Final verification

All 82 tests in the twelve affected capture/model/gather/GPU suites pass against
a fresh extraction of the published archive, with zero skips. Six native
preflight tests also pass against that extraction, and two cost-analysis tests
pass: 90 targeted tests in total. Both cost reports reproduce from raw files.
The 127 archive members and archive SHA256 were independently checked, as were
the 294 entries in the repaired earlier archive and six generated shader hashes.
Hash-bound experiment JSON/CSV files preserve their exact bytes through Git.

Final isolated replay of both redacted snapshots with the refactored native
binary matches outputs and exit context, with zero writes outside permitted
ranges. These final correctness timings are kept separate from performance
statistics. `stand-restoration.json` records the restored Wicked executable hash
matching the saved original and no remaining experiment processes.
