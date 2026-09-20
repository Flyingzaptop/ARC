# Universal adaptive optimizer execution contract

Approved scope: compute/lighting, adaptive VRS, texture sampling, ray work,
shadow/GI reuse and cadence, geometry, CPU overhead, one target-FPS controller,
quality validation and rollback. Nine hours is an estimate, not a termination
deadline. No agents. No commercial-game launches. Individual GPU runs <=60s.

The original runtime at 4c22d8a is observation plus manual uniform VRS. Core
host-provided quality actions do not constitute generic process optimization.
Neither infrastructure nor permanently declining a domain completes this scope.

## Fixed acceptance

- Same DLL on Cauldron and Wicked, without host quality callbacks, source names,
  engine labels or shader-hash allowlists as admission inputs.
- Positive native execution, unsafe-input rejection and rollback per mechanism.
- Full closed loop, changing scenes and targets 1.25x/1.5x/2x baseline.
- Moderate quality, live bounded trials permitted. Initial image limits: SSIM
  >=0.98, normalized mean error <=0.01, tile p99 error <=0.04. Natural baseline
  variation is measured separately, not used to silently relax thresholds.
- CPU steady overhead <=0.2ms/frame mean, GPU <=0.25ms/frame mean on this machine;
  probe costs are also included in end-to-end performance evidence.
- An external heavy-scene FPS gain above baseline variation is required.
  2x is a target and must not be claimed without evidence.
- Native resolution; no DLSS/FSR/frame generation; no forced game CPU affinity.
- Cached-list and in-flight generations remain safe during switch/rollback.
- User game testing remains separate from native/renderer evidence.

## Implementation order and completion ledger

- [ ] Complete resource/root/binding contracts and reversible action boundary.
- [ ] Validated shader analysis/neutral transformation and compute actuator.
- [ ] Spatial VRS and texture sampling actuators.
- [ ] Ray, shadow/GI and geometry actuators.
- [ ] Target FPS, independent quality, adaptation and CPU budgets connected.
- [ ] Two external renderers, regression matrix, net performance and packaging.

Detailed observational code must stay off the normal render path unless needed
by an enabled action. Worker compilation, disk IO and GPU waits never occur on
the render thread. Unknown bindings or dependencies prohibit a mutation.

## Checkpoint 1 (not scope completion)

Root-signature range/register-space/constant-state contracts have CPU tests.
The new shader transformer admits a bounded independent-pixel DXIL compute
class, rejects UAV reads/atomics/nonlocal writes, and retains the original
dispatch footprint while eliminating whole workgroups in coarse mode. A dynamic
CBV selects 1x1/2x1/1x2/2x2. Every output pixel, conditional write and partial edge
passed a real GPU test; one cached native command list switches and rolls back
bit-exactly. The native D3D12 debug layer reports zero errors. All 48 non-GPU
tests pass. The transform is not connected to generic process mutation yet;
physical bindings, alias safety, quality admission and the target loop remain.

Explicit opt-in shader capture exported the heavy renderer's actual bytecode.
Its expensive lighting shader accepts roundtrip, static and controlled variants
under the DXIL validator. Source shader names and benchmark labels were not
transformation inputs. This is not an external performance/quality result.

Evidence (outside source tree):
`C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/`
contains `shader-inventory`, `native-transform-01`, `native-transform-02`, and
the assembled lighting diagnostic variants. Do not publish vendor shader
binaries as ARC-owned redistributable source.

## Checkpoint 2 (experimental process actuator, not scope completion)

The opt-in process interceptor now prepares shader variants in an external
compiler worker, appends a control CBV to a compatible root signature, preserves
application root arguments, and validates actual descriptor/allocation bindings
at each submission. Placed-heap overlap and missing descriptors prevent coarse
execution. GPU command epilogues restore controls to neutral, including cached
replays. The native injected-DLL test (`generic-native-02`) verifies every pixel,
dynamic descriptor-array indexing, all rates and bit-exact cached rollback with
zero D3D12 debug errors. Observer/profiler regressions and 48 non-GPU tests pass.

Explicit diagnostic environment: `ARC_OPTIMIZER_WORKER` (absolute shader-tool
exe), `ARC_OPTIMIZER_COMPILER` (absolute pinned DXC DLL), `ARC_OPTIMIZER_CACHE`
(absolute private cache). `ArcExperimentalCompute` selects neutral/2x1/1x2/2x2/off.
Automatic quality admission remains false; this is not an accepted policy.

First external measurement (`compute-first`): baseline 78.318 FPS, neutral
77.839 FPS, diagnostic 2x1 78.244 FPS. No meaningful gain. Five shader variants
prepare, but the expensive lighting variant is declined on an unknown t12
descriptor: its declared 15-element array is not fully populated. Next work is
generic uniform control-flow/resource-index analysis; never assume an unknown
array entry is unused, and never hardcode this shader's layout or hash.

Descriptor volatility is handled at submission, not just recording; see the
[D3D12 descriptor contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1).
Captured application shader bytecode/IR caches remain local, outside Git.

## Checkpoint 3 (in progress, not an accepted optimizer)

Generic integer/control-flow abstract execution now resolves reachable resource
array indices from submission-time UPLOAD/inline constant data. Pixel-dependent
branches are explored conservatively; unavailable data/budget exhaustion decline.
Proof caching rechecks every observed uniform read. CommittedResource1 tracking
was necessary to observe the renderer's actual constant buffers. The expensive
lighting shader now passes real binding/alias admission without filling unknown
descriptors or hardcoding its layout. A GPU-address interval index avoids scanning
all textures for each CBV lookup.

Experimental shader mechanisms now include independently recognized normalized
5x5 comparison filters (9-tap alternative) and pure acyclic zero-factor regions
using the shader's existing fast-math contract. Native tests verify each pixel,
bit-exact neutral/rollback, comparison filtering against a CPU oracle, and zero
factor side-effect rejection. DXIL branch hints preserve intended control flow;
they did not produce a measurable additional win on the current external scene.

The shader worker contract is version 3; source IR used for uniform analysis is
private local cache data. `pcf9` and `zero` are additional experimental modes.
Suffix `|heaviest` delays activation until a healthy GPU profile selects the most
expensive compatible PSO. Selection uses measured costs and lifetime identities,
not engine labels or fixed shader hashes. Manual VRS+compute combination is refused
until shared submission scheduling is implemented.

External exploratory results (single pairs, not final acceptance):

- `compute-uniform`: all eligible compute passes at 2x2: 93.335 vs 77.391 FPS;
  image SSIM .89752 and tile p99 .16726 fail the moderate profile.
- `pcf-first`: 78.048 vs 78.066 FPS; image passes, no performance benefit.
- `zero-first`: 77.334 vs 77.995 FPS; image passes, no performance benefit.
- `branch-hints`: zero 77.052, pcf9 77.285, baseline 77.514 FPS; no accepted win.
- `hottest-first`: only the automatically selected hot shader, 2x2 92.419 vs
  77.360 FPS. SSIM .98150 passes, but tile p99 .04603 exceeds .04: rejected.
- Same selected shader at 2x1: 80.066 FPS; SSIM .98744, mean linear error .001170,
  tile p99 .01894 pass at this one pose. More poses/repeats are still required.
- A 1x2 selected-shader measurement is the next pending comparison.

`scripts/optimizer-quality.py` fixes independent SDR metrics: Gaussian 11x11
SSIM, sigma 1.5, encoded luma; linear RGB error after gamma 2.2; 8x8 tile p99.
The limits are unchanged. These tests are not wired into an automatic runtime
quality gate yet. No final two-renderer/target-FPS/ray/geometry closure exists.

Next priorities: finish isolated action comparisons, spatial quality protection
if needed, GPU same-input probe/rollback and target controller. Then complete the
remaining approved domains and independent renderer portability. Do not stop at
the current infrastructure/experimental actuators or claim all stages complete.

## Checkpoint 4 (spatial actuator verified; automatic session integration pending)

Isolated 1x2 is fast (90.919 FPS, SSIM .98810 at final pose) but fails SSIM at
three other poses (.9781/.9789/.9792). It is not a generally accepted setting.
The input-edge guard evaluates nine points in current, proven screen-aligned
float SRVs per macro-group; it uses no prior-frame image or engine labels.
Macro-groups with excessive contrast remain full rate. Native tests verify full
protection, coarse output, partial edges and cached rollback exactly.

Adaptive 2x2 with feature threshold .9 produced 84.060 vs baseline 77.835 FPS and
passed the final pose (SSIM .99341, mean .000805, tile p99 .02091). Independent
poses 270/420/570 also pass: SSIM .98701/.98445/.99060, tile p99
.02456/.01996/.01407. Their measured FPS were 83.10/84.71/83.76. Image limits
remain .98/.01/.04; .9 is the input-feature selection threshold, not a relaxed
quality limit. End-to-end repeatability and own CPU overhead still need closure.

Current worker contract is version 4. Modes include adaptive-1x2/adaptive-2x2,
optional feature threshold `@value`, and `|heaviest` GPU-cost selection.
The benchmark runner exposes `--edge-threshold`. Nonselected PSOs now remain
original rather than being wrapped in an unused neutral variant.

`OptimizerSession` adds tested serial target/quality/performance decisions,
generation checks, settling, incumbent retention, restoration and fault latching.
It is not yet connected to a generic runtime reference/probe acquisition path.
Do not report an automatic arbitrary-game quality loop as complete.

## Checkpoint 5 (live trial plumbing; acceptance remains closed)

`ArcStartOptimizer` / `ArcStopOptimizer` and optional `ARC_AUTO_CONFIG` connect the
target session policy to real Present cadence, GPU discovery and A/B/A image
trials. The JSON config uses absolute `python`, `critic`, fresh `output`,
`target_fps` and optional `maximum_seconds` paths/settings. The benchmark runner
exposes `--auto-target`. The implementation is experimental: CPU/GPU cost
evidence is deliberately unavailable, so it cannot retain a setting yet.

Image capture observes successful explicit `SetColorSpace1` calls. Unknown/HDR
color spaces, blank references, nonfinite pixels and failed Presents refuse live
quality admission. Three capture jobs now enqueue on consecutive Presents of
one swapchain; readback/file IO cannot delay the next capture. Policy switches
occur at those Present boundaries. This is still a temporal proxy, not exact
same-input replay: queued rendering and temporal history are not reconstructed.

`optimizer-live-quality.py` estimates bidirectional motion only from original
references (OpenCV DIS medium at up to 1920 pixels wide, one worker thread).
Candidate pixels never train the alignment. Existing image limits are unchanged;
extra confidence requirements are 95% alignment coverage, reference SSIM .99,
mean error .002 and p99 motion <=64 pixels. The worker belongs to a Windows job
that kills it on host exit, and cancellation interrupts its process wait.

Evidence in `universal-optimizer/auto-consecutive` and `auto-dis` shows successful
1800-frame (~30-second) runs around 78.30 FPS, with zero retained actions.
Consecutive capture substantially reduces motion mismatch. Offline DIS comparison
qualified one of four saved trials; a fresh live run still rejected all four
references. This is a real remaining blocker, not evidence of quality success.
Do not weaken the quality/confidence gate to manufacture acceptance.

Session evidence now expires after a bounded number of Presents even when the
target FPS is met. Candidate replacement/generation changes invalidate pending
or retained evidence. Tests cover both cases. `off` leaves future recordings
uninstrumented; `neutral` explicitly creates variants for later cached replay.

The optional DXBC conversion experiment uses the system converter and the same
mandatory DXIL validator. `dxbc-fixture` conversion does not validate, including
unmodified roundtrip. Consequently DXBC optimization is NOT supported by this
checkpoint; no driver validation is bypassed. Named entries/repeated thread IDs
are handled by the IR parser and existing native DXIL regression passes.

Checks: full Release build; 49 non-GPU tests; independent/live quality tests;
`injected-autosession-regression-01` (every output and rollback verified);
`runtime-sequence-regression-01` (native interception/lifetimes/data verified).
Automatic retention, complete overhead accounting, VRS composition, mip/ray/
geometry/temporal actuators and second-renderer portability remain unfinished.

## Checkpoint 6 (explicit mip actuator)

The generic compute worker now recognizes noncomparison `SampleLevel` operations
and supplies reversible +0.5/+1/+2 LOD bias, controlled at submission without
changing application descriptors. Texture loads and comparison samplers remain
unmodified. Invalid controls fall back to the original LOD; neutral selection
preserves its original value. The worker contract is version 5 and its control
buffer declares 48 bytes. Experimental modes: `mip-half`, `mip1`, `mip2`, with
optional `|heaviest`; the benchmark exposes their `compute-...-hot` forms.

`injected-mip-02` verifies every pixel against a CPU oracle for three actual mip
levels, half-level interpolation, cached replay, invalid controls, neutral and
rollback. Existing compute/PCF/zero/edge tests still pass with zero debug errors.
`mip-first`: 78.551 vs 77.894 FPS, SSIM .991639, mean error .000451 and tile p99
.011481. The one-pair gain is below the configured 2% minimum and is NOT an
accepted performance win. This is a bounded compute sampling actuator, not
complete automatic texture residency/streaming or pixel-shader mip control.

## Checkpoint 7 (bounded inline ray work)

Independent per-pixel inline ray queries now support the same reversible group
selection. The admitted query operations are allocation, TraceRayInline,
Proceed and committed-status reads on locally allocated query handles. Other
query operations and general DXR ray-generation pipelines remain unsupported.
AS descriptor tables resolve their GPU addresses to tracked allocation identities;
root AS SRVs use the same interval/lifetime checks. A placed output is rejected:
without the complete TLAS/BLAS input graph its indirect alias safety is unproven.

`injected-inline-ray-01` builds a real BLAS/TLAS on DXR 1.1 hardware. Every hit,
miss and output coordinate agrees with an independent geometric CPU oracle in
original, neutral, coarse and cached rollback modes, both directly and through
the same DLL. Zero D3D12 debug errors. CPU binding tests also cover null/retired AS,
root AS descriptors and placed-output rejection. This is a functionality result;
no external ray-scene performance or universal DXR compatibility is claimed.

## Checkpoint 8 (binding portability and CPU work; not full completion)

Three alternating Cauldron pairs at `repeatability-af17a7d` retained the fixed
quality limits at the final pose in every pair. Baseline FPS: 77.825, 76.925,
76.865. Adaptive 2x2/.9 FPS: 83.716, 83.133, 82.057. Paired gains: 7.57%, 8.07%,
6.76%. SSIM: .990627, .992444, .988883; tile p99: .023552, .020683, .020866.
CPU preparation increased by roughly .86–1.22ms in those runs, so the own-CPU
budget is NOT closed despite the net FPS gain. GPU lighting decreased from
about 3.59–3.64ms to 2.78–2.84ms. Nested GPU intervals must not be summed.

CPU diagnostics are now opt-in (`ARC_OPTIMIZER_CPU_TIMING=1`), with per-site
wall time explicitly including native calls, not claiming complete own-cost
accounting. Root argument storage follows actual parameter count instead of
zeroing/copying ~18 KiB on every list. Resets preserve capacity; argument lookups
borrow immutable values. A single-descriptor copy reuses interned identity
instead of allocating a vector and re-interning metadata. Reference-count,
null/unknown, self-copy, orphan and heap-retirement cases have tests.

Compute pipeline streams are decoded with alignment, size, duplicate and type
checks. The native mip fixture now creates its original PSO through that API.
Shared roots may contain unused bindless ranges; their presence alone no longer
blocks a bounded shader. For actual unbounded shader arrays, submission requires
a complete finite uniform-access proof for SRVs/UAVs/samplers. Every accessed
index is checked against the real heap and allocation. Unknown/pixel-varying
unbounded access refuses mutation. `injected-bindless-01` and
`injected-bindless-final-02` verify real uniform-index activation, varying-index
refusal and exact cached rollback. The latter also tests original HLSL [branch]
metadata: new metadata IDs account for distinct nodes, and zero-factor CFG
analysis accepts branch hints without changing their semantics.

Compute and VRS now share one original ExecuteCommandLists call, with their
independent helpers/fences composed around it. `vrs-compute-enabled-01` passes
the VRS cached/passive/cross-queue/native validation suite with compute enabled.
This is not yet an automatically validated combined quality policy.

The Wicked harness loads the exact same DLL before rendering, with the old
host-action integration explicitly OFF. First attempts prepared no variants.
Pipeline-stream coverage, bindless roots and newer DXIL validation were actual
gaps. The pinned official DXC 1.9.2602.24 package is fetched and SHA-256 checked
by `get-optimizer-dxc.ps1`; all native transform tests pass with that compiler.
`wicked-dxc19` prepares 16 variants with zero faults, but no active mutation was
selected. `wicked-discovery-after-load` moves profiling out of the loading screen;
it measures compute intervals, yet still has no selected compatible action.
Two-renderer optimization/quality/performance acceptance remains open.

The compiler queue is bounded to 256 jobs, 512 live pipeline records and 64 MiB
of pending bytecode. The earlier 64-job limit dropped much of Wicked's startup
catalog. Unsupported shaders still decline; neither capacity increases nor
successful compilation prove useful optimization.

Readonly raw/structured-buffer SRVs (including root SRVs), packed half conversion
and resource-dimension queries are now admitted as per-pixel inputs. UAV payload
reads still reject. `injected-packed-buffer-01` verifies the raw root address,
half decoding, dimension guard, coarsening and rollback against a CPU oracle.
Wicked's timed hot-pipeline diagnostics identified these missing operations;
shader hashes were used only to match diagnostic bytecode to measured records,
never to authorize a runtime transformation.

## Checkpoint 9 (actual second-renderer execution and lean interception)

Wicked exposed native interning: repeated CreateRootSignature calls returned
the same object, but ARC replaced its lifetime identity and made existing PSOs
appear incompatible. Existing immutable root/PSO/signature identities are now
preserved. `injected-interned-root-01` confirms actual native root interning on
this GPU; `profile-interned-pso-01` verifies one profile identity across repeated
PSO creation, cached execution and multiple queues.

`wicked-interned-root` executes 11,113 transformed neutral dispatches with resolved
bindings, no pool misses and no ARC faults. `wicked-coarse-first` selects an actual
measured compatible PSO and executes 6,754 coarse-control submissions / 10,034
instrumented dispatches through the same DLL, host quality callbacks OFF.
That is positive process-mutation portability. Its image quality and net FPS
gain are still unverified and must not be reported as accepted optimization.

Lean compute mode now enables only descriptor/resource/compute-binding hooks
needed by the optimizer, instead of forcing every detailed graphics/copy hook
back on. Invalidators and submission/rollback hooks remain active. Native compute
and VRS cached/passive/cross-queue tests pass in this mode. A first Cauldron pair
(`lean-hooks`) measures 83.869 vs 77.717 FPS, CPU preparation 3.616 vs 2.926ms,
submission .246 vs .100ms; CPU overhead still exceeds the requested budget.

Further CPU changes avoid redundant allocation-info queries for committed
resources, cache placed-resource sizes by complete descriptor and retained device
identity (4 devices, 256 sizes each), avoid allocating a duplicate proof-read map
on cache hits, and validate a prior sparse-binding proof before repeating the
full binding walk. `injected-proof-revalidation-01` changes the uniform output
index and replaces its descriptor with a narrower texture: the stale assumption
is not reused and the incompatible dispatch stays original. Every output is
checked and the D3D12 debug layer remains clean. New performance measurements
for these final CPU changes are pending.

## Checkpoint 10 (asynchronous proofs, DXBC and inherited state)

Uniform-access interpretation now runs on the existing background worker. The
submission thread captures at most 256 requested CPU-visible vectors and checks
completed proofs against current values. Missing inputs never become permanent
unknown cache entries; pending/over-budget analysis leaves the original rate.
The queue is bounded to 64 proof jobs and difficult cases back off. Native tests
wait explicitly for readiness and then verify changed indices, replaced views,
unsafe varying indices and rollback. `async-proof-heavy` activates after three
neutral submissions and measures 83.973 FPS, with no ARC faults.

Pointer lookup tables and unchanged descriptor-heap binds avoid tree searches
and temporary vector allocations. `cpu-pointer-index/optimized` measured 85.337
FPS (single run, not a new accepted comparison). `cpu-fast-path-29fe5c9` measured
83.590 vs 77.781 FPS; CPU preparation 3.687 vs 3.018ms. CPU budget remains open.

The DXBC path now normalizes system-converter output into public DXIL: canonical
target information, descriptor-array LLVM types matching binding counts, and
removal of genuinely unused external declarations. Validation is still mandatory.
`injected-dxbc-valid-02` compares the original SM5.1 bytecode against the validated
converted/controlled shader and checks every pixel plus cached rollback.

Resource indices can become lane-varying when pixels are remapped. Dynamic
createHandle indices therefore retain per-lane selection instead of a stale
uniformity hint. Native arrays now contain five distinct textures and exercise
both x/y group-boundary crossings, so identical test inputs cannot mask this bug.

Known ExecuteIndirect signatures preserve unrelated compute bindings and apply
the documented zero/null resets to changed constants/root descriptors. Graphics
indirect work does not erase compute-root knowledge. Explicit null descriptors
are distinguished from unobserved state and restored accurately. After genuinely
opaque work, merely rebinding the same root cannot authorize even a neutral
replacement until its arguments are known again. `injected-indirect-root-04`
verifies inherited tables, constant/CBV reset, cached execution, and refusal to
touch an opaque-state dispatch, with exact pixels and zero debug errors.

`async-indirect-heavy`: 84.065 FPS, 576 active control submissions, zero ARC
faults; comparison to the saved 29fe5c9 baseline passes SSIM .990209, mean error
.000936 and tile p99 .021215. It is a checkpoint, not full-plan acceptance.
Automatic quality provenance/retention, complete overhead accounting, geometry,
temporal GI/shadow reuse and the final two-renderer matrix remain unfinished.

## Checkpoint 11 (live reference confidence and measured rollback)

Live trials now record policy epochs and active submission counters. A candidate
that fell back to unchanged work cannot be accepted as an exercised change.
This is submission provenance, explicitly not proof of a same-frame GPU dataflow.
Rollback confirmation waits on the control fences from the background worker;
it is no longer inferred merely from requesting `off`. Per-trial decision JSON
preserves timing, quality, provenance and cost-availability outcomes.

Optional trial-only state sampling discovers literal float CBV loads from shader
IR and reads at most 64 vectors from original CPU-visible game constants. It uses
no variable names and is disabled outside capture windows. Consistent changes
across those values estimate the simulation fraction between original references,
instead of assuming Present intervals equal simulation steps. Candidate pixels
never fit that estimate. Mismatched identities, stale submissions, nonfinite or
inconsistent state fall back to the timing proxy.

Motion ambiguity on locally flat surfaces is handled using agreement between
both ORIGINAL warped references: each 3x3 luma range must be <=2/255 and their
maximum linear channel difference <=.001. The final full-image quality limits,
95% confidence coverage and reference SSIM .99 remain unchanged. Flow consistency
and flat-region confidence are reported separately. Independent image-damage,
scene-cut and state-mismatch tests pass.

`auto-reference-confidence` completes a 3000-frame, 44-second dynamic run. Five
successive trials pass image quality (SSIM .99548–.99861, confidence coverage
.96518–.98171). Retention remains blocked by unavailable complete cost evidence;
this must not be described as a finished autonomous optimizer. The runner still
enforces a hard 60-second per-process limit; its frame cap is now 3600.

A shared-memory edge-scan experiment preserved native outputs but did not improve
measured lighting time (2.811 vs 2.783ms in the preceding run). It was reverted;
its evidence remains under `shared-edge`. Descriptor value/reference lookup uses
hash tables with full-value equality, and descriptor mutation/submission locking
is separated from other state updates. `runtime-hash-ledger-01` passes lifetime,
copy and output checks; `descriptor-locks/optimized` measures 83.525 FPS and
3.680ms CPU preparation. No claim that the CPU budget is met is justified.

## Checkpoint 12 (measured control costs and applicable live candidates)

Optional `ARC_OPTIMIZER_GPU_CONTROL_TIMING=1` measures policy-upload copies and
their barriers with timestamp queries. Readback occurs only on the background
collector after the submission fence completes. Four independently retired
slots prefer already-consumed samples; exhaustion drops a diagnostic sample
instead of delaying rendering. Counts distinguish collected/dropped/invalid
samples. This excludes shader guards, neutralization and query overhead and is
explicitly NOT complete GPU overhead evidence. `control-cost-02` collected all
577 samples without drops: 1.10387ms total, approximately .00191ms per helper.
The CPU diagnostic separates original native submission time and entry-lock
waiting; it still is not complete optimizer overhead evidence.

The optimizer state lock no longer spans the original driver submission.
Descriptor mutation/submission ordering stays serialized, while shared control
ownership prevents pool reuse until retirement is signaled. A prepared active
helper cannot report itself retired; device removal cannot count as completion.
`injected-unlocked-submit-01` verifies exact native outputs and cached rollback;
`vrs-unlocked-submit-01` passes cached/passive/cross-queue regression with compute
enabled. The new GPU-control test covers pending data, direct/compute queues,
unread ring reuse, neutral operation, disabled timing and debug-layer errors.

In the diagnostic heavy-scene pair, recording-reset lock waits fell from 43.09
to 10.62ms over the process and lifetime-release waits from 41.68 to 20.72ms.
FPS was 84.51 before and 84.74 after; preparation was 3.742 vs 3.775ms. This
establishes less lock contention, NOT a significant end-to-end CPU/FPS gain or
closure of the .2ms budget. Both runs include optional diagnostic overhead.

Automatic candidates now derive readiness from the selected compiled shader's
capabilities. Unsupported PCF/zero/edge/mip policies do not consume image trials.
Evidence carries the selected pipeline generation; replacement invalidates an
in-flight trial, and a previously rejected action may retry on a new generation.
No observed active submission bypasses the expensive critic and rejects the
trial. The fixed quality thresholds remain unchanged.

Cadence measurement follows image analysis and uses incumbent/candidate/incumbent
windows. More than 10% disagreement between incumbent windows makes performance
evidence incomplete; their difference also enters the gain/noise gate. This
prevents scene changes during the critic's execution being mistaken for savings.
It does not establish same-frame GPU image provenance or total probe cost.

`wicked-auto-bracketed-01` uses the SAME DLL, with host quality actions off, in a
44.6-second process. Four real trials (1x2, 2x2, mip-half, mip1) pass the live
image critic and retire their policies without ARC faults. Their bracketed
candidate times are 1.711/3.514/2.496/21.086ms against respective incumbent means
1.616/3.338/2.476/20.986ms: NONE demonstrates a speedup. Earlier unbracketed
Wicked timings must not be treated as accepted performance evidence.

The runners record compiler/DLL hashes and explicit diagnostic settings; Wicked
can request bounded automatic trials and 1..35 measured seconds, retaining the
hard 60-second process timeout. Full retention remains blocked by unavailable
complete cost evidence. Geometry, temporal GI/shadow reuse, automatic spatial
VRS, complete overhead/provenance accounting and final target matrix/packaging
remain unfinished. This checkpoint does not complete the execution contract.

The updated Cauldron automatic run `auto-capabilities-bracketed-01` completes
3000 frames in a 45.93-second process, at 74.94 FPS INCLUDING live probes. Four
trials pass image/retirement checks; adaptive 1x2 measures 12.417ms against a
bracketed 13.000ms incumbent, but complete costs are still unavailable and it
is not retained. This whole-run result is slower than the no-DLL baseline.

Profiling the independent critic identifies its NumPy Gaussian and per-tile
Python loops as the main CPU expense. The live worker now uses OpenCV's
float64 separable filter with the SAME 11x11 sigma-1.5 kernel/reflection border;
tile sums are vectorized with correct partial-edge denominators. Native image
resolution, DIS flow resolution and every acceptance threshold are unchanged.
On the same recorded trial, cProfile wall time decreases from 4.763 to 2.496s.
SSIM changes by <1e-15 from summation order, linear mean/peak errors are identical.
Independent Gaussian/tile oracles and damage/scene-cut/state-mismatch tests pass.
This accelerates validation, not rendering by 2x.
