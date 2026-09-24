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

## Async chain follow-up

Apply `async.patch` on top of the recorded synchronous experiment. Copy
`ArcAsyncObjectChain.h` into Samples/Tests and `ArcChainContract.h` into WickedEngine.
Use the same Release/x64 build command and MT ARC libraries above.

Keep ARC_CONTROLLED_CULL=0. Set ARC_ASYNC_CHAIN=1 for the source-assisted chain,
2 for its exact CPU oracle, or 0/unset for the original path. ARC_CHAIN_CHUNKS=1
submits the whole natural object batch; 2 partitions it into two disjoint batches.
A one-object scene uses one packet even when two were requested. No frames are
added to the renderer queue; each batch is consumed in the same Scene::Update.

`analyze_async.py <raw-directory>` produces comparison.json and frametimes.svg.
`test_analyze_async.py` checks packet-level versus frame-level attribution and the
first-consumer boundary. Do not sum submit-to-consume gaps as independent CPU work.

The extension replaces AABB corner transformation, center calculation, visibility
and index compaction for admitted rigid objects. It does not update world matrices
or generate all draw/instance data. Geometry/metadata stay resident between calls;
changed metadata spans are uploaded. Current matrices and required CPU outputs
still cross the bus. GPU shader execution can contend with real rendering.

Source-reviewed supported ordering excludes skinned/dynamic meshes, soft bodies,
emitters, hair and impostors. The new deferred CPU job captures the result pointer
BY VALUE. Result storage survives the existing jobsystem Wait; each slot retires by
its own fence before reuse. Exact scene/count/frustum match gates mask/list reuse.
Device recreation and production stop/recovery are not provided by this experiment.

The measured result is negative; no automatic placement/fraction controller was
added. The original Tests.exe has been restored. The source-assisted binaries and
raw captures remain local at the paths in the async manifest.

## 2026-09-24 correction

The object-job lambda must capture the block-local `replaced` BY VALUE:
`[&, replaced](wi::jobsystem::JobArgs args)`. Its type being int did not fix its
lifetime. The block ends before the enclosing UpdateVisibility Wait(ctx).

The published foundational patch now includes that fix. Reapply the NEW
wicked.patch and then the NEW async.patch to their documented bases. Copy
FinalListCheck.h alongside ArcControlledVisibility.h into Samples/Tests.
`ARC_VERIFY_FINAL_LIST=1` checks the actual consumer indices AFTER Wait(ctx),
before resizing, against independent current AABB/frustum/layer CPU membership.
It detects count errors, missing/foreign/duplicate/out-of-range indices. Keep it
off in timing runs. The portable final_list_tests.cpp tests the checker itself.

Historical raw CSV archives are now committed under both 20260923 evidence folders.
Their bytes match the original archive hashes. Historical timings predate the
lifetime correction and are retained for audit, not as validation of corrected code.
