# CPU-to-GPU offload experiment

Plan: `ARC_CPU_GPU_OFFLOAD_AGENT_PLAN.md`, 2026-09-23. Scope authorized now: O0–O1.

## Base and preservation

- Accepted prototype base: `afca857`; M0–M4 remain accepted within their scope.
- Incomplete M5–M7 saved and pushed as `41a5434` on
  `codex_den/m5-m7-checkpoint-20260923`. No claim of M5–M7 completion.
- Offload branch: `codex_den/cpu-gpu-offload`, based on the accepted prototype.
- No old full test suites or commercial games are being run.

## Gate state

| Gate | State | Evidence / next action |
|---|---|---|
| O0 | Screen complete; admission blocked | Five regions ranked, two investigated. Visibility selected for boundary analysis; no automatically admissible bulk operation. |
| O1 | BLOCKED_SEMANTICS | Complete memory, effects, concurrency and consumer boundary not recovered. No GPU kernel generated. |
| O2–O4 | Not started | Not authorized as the current primary task; placement/joint control also require a positive O2 gate. |

The current CPU decoder has no loop/memory/job extractor. Source and symbols may
explain measured work or provide an oracle; they cannot select or authorize a
production substitution. No named engine adapter or hand-coded application offset
will be used to turn a blocked candidate into a successful result.

## Predeclared economic screen

Retain the plan's gate: required gain is max(0.5 ms, 5% of baseline complete-frame
mean, 2x baseline drift); conservative gain is best baseline minus worst active
run. At O0, even the optimistic removable CPU critical-path work must exceed the
0.5 ms / 5% floor. Missing drift or critical-path evidence remains unknown.

A/B/C final performance trials are not justified until an admitted complete
operation exists. O1 kernel time alone is not a win; transfer, waits, restore and
continuing instrumentation costs must count. Exact CPU FP semantics and complete
memory ownership are required. A blocker or negative result stops this branch of
expansion rather than restarting unrelated M stages.

## Current evidence

- Existing Wicked Instances attribution suggested CPU-side headroom (historical
  52.98 FPS and 5.98 ms median GPU span), but it is not a synchronized critical-path
  proof and is not a current performance result.
- Pinned DXC and DynamoRIO installations remain available. Hardware query reports
  NVIDIA RTX 3060 Laptop, driver 616.64. Current queried power limit is unavailable;
  the user's 30 W setting is not replaced with an invented measured limit.
- Fresh diagnostic: `.../universal-optimizer/offload-o0-20260923/cpu-screen.csv`,
  existing profiler mode, 15-second cap, normal 65,536-instance scene. No ARC DLL,
  no DBI, no workload-size edits. This is a screen, not final arm A.

## Terminal result: BLOCKED_SEMANTICS

This completes the bounded screen, not O0 admission or O1 implementation.
No CPU invocation was replaced; no performance benefit is claimed.

| Diagnostic CPU region | Mean wall ms | Decision |
|---|---:|---|
| Application Render | 16.466 | Command/API work; not an isolated transferable computation |
| Frustum Culling / visibility | 2.194 | Selected; shared effects and CPU consumers block admission |
| Instance animation/update residual | 1.819 | Alternative investigated; memory/job and exact FP closure missing |
| SubmitCommandLists | 1.474 | Below optimistic screening floor; API/wait work |
| Update Buffers (CPU) | 0.221 | Below optimistic screening floor |

After 6 s warmup: 296 adjacent CPU submission intervals, mean 30.404 ms,
p50 30.115 ms, p95 42.453 ms, p99 52.596 ms. These are CPU submission
completion intervals, NOT display-completed FPS or an O2 baseline. The provisional
5% floor is 1.520 ms; baseline drift and GPU criticality are unknown. Removable
critical time ranges from zero up to each aggregate wall interval, before costs.
There is no positive lower bound or established transfer break-even point.

### Why the two candidates stop here

1. Visibility (`wiRenderer.cpp:3640`, oracle RVA `0x1cba20`) dispatches jobs,
   compacts output with atomics, updates occlusion state and can lock/create
   resources. CPU consumers use visible indices (`wiRenderer.cpp:5944`).
   `CheckBoxFast` is a six-plane per-object predicate, not the whole timed region.
   Extracting only it does not remove the original expensive job. Candidate inputs
   include 32-byte AABBs and a 96-byte frustum but extend beyond them; full ranges
   and writer ownership are not established. No GPU-resident-only route is valid.
2. Instances (`Tests.cpp:609`, enclosing oracle RVA `0xa1060`) runs two jobs,
   writes matrices/colors and waits. Approximate nominal outputs are 4 MiB of
   matrices plus 1 MiB of colors; these are source estimates, not captured ranges.
   CPU sin/pow, matrix arithmetic, memory effects and concurrency must be preserved.
   A handwritten HLSL equivalent selected by engine symbols would bypass admission.

The existing decoder admits register MOV/LEA, not these loops, memory accesses,
SIMD FP, calls, task ownership or consumers. Source/PDB inspection is explanatory
only. The matched PE/PDB identity validates symbol offsets, not source provenance
or runtime semantics. Source hashes are saved independently.

### Verification and artifacts

- Offline DIA tool compiled/linked and successfully validated the Release PE's
  GUID/age against its PDB; exact function RVAs saved for diagnosis only.
- One short existing renderer run completed successfully. No commercial game,
  modified scene size, CPU offload, ARC DLL or DBI was used.
- `python scripts/test-offload-screen.py` checks nested-timer arithmetic, duplicate
  handling, screening floor and preservation of unknown evidence. It does not
  test an offload runtime, which has not been implemented.
- Manifest: [CPU_GPU_OFFLOAD_O0_O1.json](evidence/CPU_GPU_OFFLOAD_O0_O1.json).
  Compact results: [economic-screen.json](evidence/offload-o0-20260923/economic-screen.json).
  Raw CSV and disassembly remain local at paths/checksums recorded in the manifest.
- Changed paths are diagnostic tools plus this record/evidence; runtime untouched.

### Next capability required

A bounded automatic bulk-region extractor with complete memory/ownership and
consumer admission is needed before GPU code generation, specifically for the
selected visibility predicate plus its surrounding compaction/dependency boundary.
Its absence is not solved by GPU submission or additional PID control. This plan's
stop condition applies: stop expansion here; do not resume M5–M7 or claim O1 done.

## Follow-up: authorized source-assisted economic experiment

The user explicitly separated profitability from automatic extraction after reviewing
84bb25c. Source-assisted replacement is allowed for this control experiment only.
Implemented predicate offload and a wider predicate + compaction variant in the
existing Wicked workload. Both failed to improve measured CPU submission cadence;
no improvement in p95/p99 was confirmed. Full GPU-completed-frame O2 was not measured.
Original EXE restored, experiment default off. Automatic O0 admission/O1 remain blocked.
See [controlled experiment report](reports/2026-09-23-controlled-visibility-offload.md).

## Follow-up: larger asynchronous chain

Implemented a source-assisted matrix-to-bounds/center/visibility/compaction chain
(one or two natural disjoint object packets per frame), deferred CPU consumption,
resident metadata and sparse metadata updates. Matrices remain CPU-produced.
Exact native oracle passed on the observed scene after correcting startup partition
and deferred pointer lifetime faults. Final arithmetic/protocol evidence and faults
are retained. Two packets reduced consumer wait but both variants slowed measured
CPU submission cadence. Full GPU-completed-frame gate remains unavailable.

Terminal result: negative on this measured workload, original EXE restored, default
off. No universal extractor or automatic fraction controller added after failure.
[Async chain report](reports/2026-09-23-async-object-chain.md).

## Review correction — 2026-09-24

Confirmed a separate block-lifetime defect: replaced left ALLOW_OBJECTS scope
before the enclosing Wait(ctx), despite being int. Published wicked.patch now uses
[&, replaced]; async.patch is regenerated on that corrected base. The actual final
CPU consumer list is checked after Wait and before resize in diagnostic runs.
Three native post-join checks and checker/publication regressions passed. Historical
performance captures remain historical, not correctness evidence for the fixed code.
Raw CSV ZIPs are committed for both old series and the new targeted verification.
See [correction report](reports/2026-09-24-replaced-lifetime-fix.md).

## Resident draw-consumer follow-up — 2026-09-24

A different closed suffix (draw preparation -> GPU draw consumer) produced a positive
CPU submission-cadence result on Wicked: 19.40 -> 11.30 ms. This does not reverse the
negative result of the earlier bounds/readback chain and is not universal ARC.
Pixel/depth/record oracles passed. Actual active power was ~62-65 W, not verified 30 W;
display FPS is unmeasured. Original EXE restored, default off.
[Current report](reports/2026-09-24-resident-draw-consumer.md).


## 2026-09-24 — full ARC integration comparison

Session-bound Wicked resident adapter and generic GPU optimizer were run together.
Wicked A-B-B-A: 40.83/45.82 Present Hz without DLL versus 76.30/73.38 with full ARC;
CPU through submission averaged 23.14 versus 13.36 ms. These are Present-call rates,
not display FPS. Native oracle: 517 frames / 33,782,848 records, zero ID/depth errors;
stop restored the CPU branch.

Cauldron CPU offload remains unsupported. A-B-B-A did not prove repeatable improvement;
GPU candidate selection is blocked by capture capacity declines on both hosts. No
nonzero GPU simplification policy was applied. The one Cauldron controller-off ablation
was inconclusive against changing GPU clocks/temperature. Do not claim universal
CPU extraction, universal GPU optimization, or a finished general-game product.

[Report, plots, raw measurements and reproduction](reports/2026-09-24-full-arc-comparison.md).


## 2026-09-24 — automatic CPU discovery gate

From `1c8a51e`, added a bounded external activity/context observer and name-blind
machine-code backedge/memory/recurrence screen. Final capture: 376 contexts,
19 main-image function regions, 12 overlapping candidate spans, **zero admitted tasks**.
A conditional 16-byte-record exchange was recognized structurally, but invocation
bounds, alias/dependency closure, ownership and first consumers remain unknown.
No GPU replacement, eliminated CPU invocation or transfer occurred. The automatic
profitable-offload objective remains unfulfilled; the specified discovery stop gate
was reached. No additional engine adapter or broad scheduler was substituted.

[Exact candidate, raw data, timing and missing mechanism](reports/2026-09-24-automatic-cpu-discovery.md).


## 2026-09-24 — whole-call contracts, training mode

The context observer budget now accounts failed attempts (`a7e29cf`). Added bounded
native entry/return/continuation instrumentation and lossless streamed training traces.
Recovered one top-level 65,344-record ordering invocation with complete observed
user-mode memory coverage and matching input/output record multiset. The stable-sort
counterexample differs at 53,871 positions on the final input. A same-thread consumer
was located at RVA 0x1c9e94, offset +4 in the output span; global ownership is not proved.
All six candidate entry regions were investigated; four have observed atomic effects,
and another has unresolved external effects. Chained unwind fragments are now grouped.

No GPU work or replacement was enabled. Machine contracts deliberately retain unknown
all-path, ownership/lifetime and exact tie-order conditions. The manual adapter's speedup
is not included. This is completion of the capture/evidence pass, not acceptance of a
safe automatically transferable task.

[Report and machine-readable contracts](reports/2026-09-24-cpu-whole-task-contracts.md).

## 2026-09-24 — contract refinement without another full trace

Reused `4017d03` evidence. All 53,871 stable-sort differences are within equal-key
groups; observable output equivalence remains unknown, not failed. Added CFG
must-constant checks for the four inferred comparison sites, bounded snapshot
preflight and 744 isolated original-binary insertion-path cases (zero mismatches).
The real 65,344-record input does not pass the restricted path guards.

Atomic-containing parents now have a separate inner-loop inventory: index generation
and bitmask packing can be investigated while retaining surrounding CPU effects.
Allocation ownership/publication and complete live-out semantics remain open;
no live dispatcher or automatic GPU replacement is enabled. Maximum recorded
training Present interval is 57.61 seconds, so the full tracer stays diagnostic.

[Refinement, remaining proof obligations and reproduction](reports/2026-09-24-cpu-contract-refinement.md).

## 2026-09-25 — large exact-order model and independent admission axes

From `037d00b`, reproduced the full 65,344-record permutation, including equal-key
ordering, with a deterministic model and the saved original machine code. All 49
native differential cases (1,074,241 records), including heap fallback, agree.
The isolated oracle uses the complete grouped 519-byte heap helper, not the old
18-byte prologue-only capture. No new full trace or renderer run occurred.

Reconstructed the TLS vector's allocation/growth, fill, sorting, consumption,
reset/reuse and growth-time retirement from the existing image. Birth/generation,
all aliases and exclusion through commit are still unknown. Added executable
separate semantics/access gates and generation-bound dispatch/commit tickets as
a contract checker; no trusted live provider or dispatcher is implemented.

The full algorithm model is differentially validated, not certified for every
machine path/continuation. Both live admission axes remain closed. Inner-loop
frequency/cost are not inferred from parent-function timings or buffer creation.

[Report, actual gate result, lifecycle and exact-order oracle](reports/2026-09-25-cpu-large-operation-admission.md).

## 2026-09-25 — isolated GPU sort economic check: negative

Implemented an exact-permutation GPU port on private buffers, with parallel
disjoint child partitions. 26 GPU invocations / 1,453,952 checked records agree
with the original native CPU code, including distinct-payload equal keys and heap
fallback. No game, live replacement or full tracing was involved.

On the saved 65,344-record array, two interleaved series measure CPU medians
4.134 / 3.427 ms versus full GPU-route medians 64.010 / 60.369 ms. GPU sorting alone
costs 62.961 / 59.272 ms; the root partition alone costs about 13 ms. Transfers
are not the main loss. **Do not build ownership machinery for this standalone
sort on this result.** A wider consumer chain or another found candidate needs
its own bounded economic check; earlier manual adapter wins are not inherited.

[Measurements, exact GPU implementation, raw data and decision](reports/2026-09-25-exact-sort-gpu-economics.md).

## 2026-09-25 — parallelism-first automatic diagnostic selection

Exact-sort development remains stopped. Replaced exchange-first capture selection
with map/scan/reduction feasibility triage, explicit transfer/economic unknowns,
overlap tracking and manual-evidence separation. Screened 43 saved intervals;
automatic first choice is the record-processing/packing consumer. Unsupported
exchange groups are omitted. Deep capture requests without suitable evidence
become bounded entry/return observation, not another full instruction trace.

The selected parent's new native boundary check measured 5.3223 ms wall time over
one call; this is not isolated useful packing time or measured frequency. No GPU
implementation is economically approved yet. Fixed fixture readiness timing after
an initial loading-time miss. Existing tracer and exact-sort experiment are unchanged.

[Shortlist, detailed selected-candidate review and limits](reports/2026-09-25-parallel-first-candidate-screen.md).
