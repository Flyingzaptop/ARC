# ARC worker placement

This experiment changes scheduling preferences for ARC-owned workers. It does
not turn sequential game code into parallel code, change hard process affinity,
or reserve a CPU exclusively from Windows and other applications.

## Modes

`ARC_WORKER_PLACEMENT` selects `normal` (default), `prefer`, `core`, `partition`,
or `adaptive`. The owned Cauldron/Wicked runners expose `--worker-placement`.

* `normal`: retain original CPU sets and ideal-processor settings.
* `prefer`: supply an ideal-processor hint for ARC threads and the primary
  thread of ARC child processes; preserve their available CPU sets.
* `core`: restrict ARC workers/children to the CPU sets of one physical core,
  including its SMT siblings. Game CPU sets stay unchanged.
* `partition`: additionally exclude that core from the game's **default** CPU
  sets. Explicit thread assignments may override defaults, and hard affinity
  still wins. This requires `ARC_WORKER_ALLOW_PARTITION=1` and at least six
  available complete physical cores. The test runner opts in for this mode.
* `adaptive`: compare normal/candidate/normal frame windows; retain a placement
  only after two confirmations. Defaults to hint/core trials. Partition trials
  additionally require the explicit environment opt-in above.

The candidate core is selected from Windows CPU-set topology, respecting the
initial process affinity/default CPU sets and excluding parked/foreign-reserved
cores. Lower efficiency classes (efficient cores on heterogeneous machines) are
preferred; the final tie-break is a stable high core index. This is a candidate
heuristic, not a claim that this core is optimal. Multi-group systems currently
fall back to normal scheduling rather than narrowing game access incorrectly.

## Lifetime and rollback

All three ARC-owned threads and live compiler/critic child processes register
with the placement manager. Child placement is set before resuming their primary
thread. Compiler and critic processes run below normal priority.

`ArcWorkerPlacement` accepts a mode as a wide string. `ArcStopOptimizer` also
returns placement to normal. Original settings are restored on mode changes and
lease retirement. If another component changes the game default CPU sets while
ARC owns a partition, ARC preserves that newer assignment and refuses another
partition. Hard-affinity changes invalidate non-default placement. Failed
restorations remain visible as `rollback_pending`; adaptation stops after faults.

Adaptive sampling never waits on a mutex in Present. A missed sample invalidates
the window. OS setting changes run on the background collector. Each measurement
window has 120 frames, with settling after changes. Acceptance requires either
at least 2% and .05ms mean improvement, or at least 10% and .2ms p99 improvement
with mean regression no greater than .5%. Both p95 and p99 must remain within 2%
of the bracketed baseline. Baseline drift rejects a comparison. Two confirmations
are required; retained evidence expires after 12 windows. Large regressions,
two seconds without Present progress, and changed GPU policy/pipeline identity
restore normal scheduling and invalidate the trial.

## Evidence and limits

Pure tests cover SMT, heterogeneous classes, affinity exclusions, small CPUs,
groups, repeated acceptance, drift/tail rejection, expiration and rollback.
Native tests verify actual execution on the chosen core, child placement before
execution, game-thread defaults, restoration, ownership conflicts, workload
invalidation and the idle watchdog. Compute transformation/rollback native tests
also pass with both `core` and `partition` enabled.

Initial continuous benchmark results are **not accepted as mode comparisons**:
GPU temperature rose from 55C to 87C and clocks fell from about 1850 to 1390MHz.
The normal reference itself fell from 78.86 to 61.16 FPS; the next run ended with
2480/3000 frames. The runners now optionally wait for three idle temperature
samples (`--max-start-temperature`) without modifying fans, clocks or power.
Every game process retains its hard 60-second cap. Performance claims require
complete, interleaved repetitions under comparable thermal conditions.

CPU placement acceptance is separate from graphics-quality policy acceptance.
The full graphics optimizer still lacks complete GPU-overhead evidence and does
not automatically retain graphics mutations. No universal or 2x FPS claim is
made by this worker-placement feature.
