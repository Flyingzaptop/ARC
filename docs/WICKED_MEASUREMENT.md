# Wicked measurement hardening

`ArcWickedTelemetry.h` records separate read and write presence per resource per command list. Repeated accesses of the same direction are deliberately coalesced. These summaries are not ordered dependency traces and cannot establish write/read causality for Stage 16.

The official harness exports `hook_timings`: cumulative calls, nanoseconds and 64 fixed log2 nanosecond histogram buckets for exported hooks. IDs: 10 create, 11 destroy, 20 SRV, 21 UAV, 22 sampler, 30 begin, 31 resource use, 32 barrier, 33 copy, 34 draw/dispatch count, 40 submit, 50 present. Scopes start before breadcrumb/host/bridge calls and include mutex wait. Bucket 0 covers 0–1 ns; bucket k covers [2^k, 2^(k+1)-1] ns. Storage is constant; concurrent snapshots are approximate. Summed hook durations are worker thread time, not elapsed frame critical-path cost. The histogram accounting itself is excluded from its own duration. Existing Present and governor timing remains available.

## Repeated three-arm comparison

Build the Wicked integration using `apply-wicked-engine-integration.ps1`, which copies the new telemetry header, then run:

```powershell
./scripts/compare-wicked-arms.ps1 -Executable C:/path/to/Tests.exe -WorkingDirectory C:/path/to/WickedEngine/Samples/Tests -SourceSha <built-ARC-commit>
```

The script requires an existing binary built from the declared source. It does not rebuild or establish binary provenance itself. Raw JSON for every process is retained, and summary ratios weight each scene/repetition equally. Default six repetitions use all six OFF/observe/adaptive order permutations and rotate scene order identically within each triple. Each process prewarms all scenes and excludes settling frames. A separate initial observer calibration supplies one fixed target and profile baseline for every adaptive run. Per-scene p50/p95 GPU and CPU frame intervals are retained; ratios use p50. Repetition/order data must be inspected for instability; rotation reduces systematic bias but does not remove all thermal or background-load effects.

- OFF never constructs a NativeHostAdapter; native hooks return immediately. The common scene/profiler harness and trivial hook call/branch cost remain, so this is not an unpatched-engine comparison.
- Observe enables observation at full quality.
- Adaptive enables the existing quality controller against the external calibration during measurement. It resets renderer quality on exit, but does not validate governor recovery or image equivalence.

Comparative runs disable hook histogram instrumentation (`ARC_WICKED_HOOK_TIMING=0`) to avoid clock/atomic perturbation on hot paths. Collect detailed official-run hook diagnostics separately. This experiment emits `acceptance_evaluated=false`; it does not replace or relax Stage 14.5/15 acceptance. GPU timing improvements alone do not establish acceptable image quality.

Direct experiment environment: `ARC_WICKED_EXPERIMENT_MODE=off|observe|adaptive`, `ARC_WICKED_SCENE_OFFSET=0..4`, and, for adaptive, positive integer `ARC_WICKED_TARGET_US`/`ARC_WICKED_BASELINE_P50_US`. Leave experiment mode unset for the official stage harness.
