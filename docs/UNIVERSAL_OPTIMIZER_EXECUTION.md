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
