# Source-assisted Wicked experiment

Not a universal ARC backend. Default mode 0 preserves the original CPU path.
The original saved Tests.exe was restored after the measurements.

## Files

- `ArcControlledVisibility.h`: source-assisted CPU/GPU predicate and GPU compaction.
- `wicked.patch`: ONLY this experiment's changes against the already instrumented
  local Wicked renderer and bridge. It does not contain prior local harness work.
- `analyze.py`: diagnostic submission cadence, raw events, comparison and SVG.
- Report: `../../docs/reports/2026-09-23-controlled-visibility-offload.md`.

## Reproduction on the recorded local harness

Use `git apply --check` before applying `wicked.patch` in Wicked. Copy the header
into Samples/Tests. The existing local harness changes are prerequisites; the
patch is not intended for an arbitrary clean upstream Wicked checkout.

Build Samples/Tests/Tests.vcxproj with VS18 MSBuild, Release/x64, explicit
SolutionDir pointing to the Wicked root with trailing slash, ArcRepoRoot pointing
to ARC-perceptual, and ArcBuildRoot pointing to its **build-wicked-generic**
(MultiThreaded CRT, not the ordinary MD build directory).

Process environment `ARC_CONTROLLED_CULL`:

| Value | Mode |
|---|---|
| 0 / unset | Original CPU path |
| 1 | Serial CPU predicate batch diagnostic |
| 2 | GPU predicate with synchronous CPU result delivery |
| 3 | Mode 2 plus full CPU predicate oracle |
| 4 | GPU predicate + visible-index compaction |
| 5 | Mode 4 plus predicate and exact visible-set oracle |

Use existing scripts/profile-wicked-cpu.ps1 with Scene 18, Seconds 15, Mode off,
and AlwaysActive. GPU oracle modes are diagnostic only; never include them in
performance arms. Execute all arms at the canonical Tests.exe path; one renamed
original EXE run produced no samples and is explicitly invalid. Exclude startup
identically using the analyzer's predeclared 6-15 second window.

`python experiments/wicked-offload/analyze.py <raw-directory>` regenerates the
comparison and graph. Raw paths, binary hashes and archive hash are in the manifest.

This prototype aborts its own test process on D3D12 failure/5-second fence timeout,
not a production recovery path. No arbitrary input or numerical-domain guarantee.
Modes 4/5 assume the observed opaque-instance workload permits parallel list order.
The GPU uses the real renderer's device, a dedicated compute queue and one in-flight
operation. Every invocation waits for CPU consumers. No pipelined multi-packet
scheduler has been implemented or evaluated.
