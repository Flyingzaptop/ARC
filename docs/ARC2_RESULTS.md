# ARC2 results

Status: working experimental vertical slice; full ARC2 milestone is not accepted.
The application-facing state/IR boundary and owned exact rewrite work. Strict
five-codebase transparency, the CPU cost gate and migration of the wider legacy
optimizer remain open. No independent-engine net acceleration is established.

Repository baseline: `f72b02c`; initial IR: `0fa02a2`; first measured frontend:
`324081d`. Measurements from distinct frontend builds must not be pooled.

The full external matrix uses the frozen `014781d` DLL SHA-256
`e8b3917c5285343a55eaa13df8eac850db50c3cbc4bf52533a5f3415731b84b6`.
The delivered `02a6d33` DLL SHA-256 is
`c7585b01eccc108867a45104b779055d6c19d1ffcf2dcf5cf04cb62dee0ef215`.
It adds typed command arguments and basic IA/scissor fields; its separate owned
tests and targeted external checks must not be substituted for a full performance
matrix of that newer binary.

## Confirmed implementation evidence

The owned native fixture passes API construction, COM parent identity, descriptor
and root binding, real compute readback, mixed-alignment pipeline streams,
PrivateDataInterface ownership, Present, ResizeBuffers and shutdown checks.
A generic exact repeated-clear rule removed 5,080 native clear calls in the
40-frame evaluation fixture. Reference/modified/reference readbacks were identical;
the existing PerceptualCritic accepted the measured GPU-region change. A separate
same-process disable test restored forwarding for the second half of the run.
This is an owned correctness probe, not independent-engine acceleration.

The owned ARC2 IR test passes with assertions active in Release. Three ARC2 native
GPU fixtures pass, including exact Dispatch arguments in the new schema. The
existing non-GPU/non-stress CTest selection passed 63/63. Final GPU/stress
regressions passed 26/26 (25 GPU and one stress test), recorded in
`final-gpu-stress-tests.txt`. These selections cover different risks and must not
be described as exhaustive interface or engine coverage.

## Ten-workload frozen matrix

Each workload has two recorded A-B-C-D-D-C-B-A rounds, with ten seconds of
startup/warmup exclusion and twelve seconds requested measurement. The strict
analyzer excludes invalid arms; it does not repair absent frames. Values below
are median successful application Present-return Hz, **not display FPS**.

| Codebase / workload | A native | B passthrough | C observe | D optimize | Final interpretation |
|---|---:|---:|---:|---:|---|
| Cauldron Brixelizer GI | 59.99 | 20.09 | 18.89 | 19.08 | UNSUPPORTED IID transparency; large negative diagnostic cost |
| Cauldron CACAO | 59.99 | 36.87 | 35.29 | 36.19 | UNSUPPORTED IID transparency; negative diagnostic cost |
| MiniEngine Sponza | 144.03 | 144.03 | 144.03 | 144.02 | NEUTRAL cadence under 144 Hz cap; overhead gate unproven |
| MiniEngine CesiumMan | 144.03 | 144.02 | 144.00 | 144.03 | NEUTRAL cadence under 144 Hz cap; overhead gate unproven |
| Diligent Helmet | 1783.61 | 1801.94 | 1741.38 | 1744.98 | INVALID causal inference: native bracket drift 23–30% |
| Diligent Compute, real time | 1793.24 | 1640.41 | 1616.08 | 1704.29 | NEGATIVE descriptive result; D bracket-adjusted -5.55%, no admitted rewrite |
| bgfx 8,000 draws | 335.75 | 34.29 | 33.38 | 32.50 | UNSUPPORTED IID transparency; D bracket-adjusted -90.34% diagnostic |
| bgfx deferred | 144.03 | 143.78 | 141.52 | 141.90 | UNSUPPORTED IID transparency; cap limits performance inference |
| Wicked 65k Instances | — | — | — | — | INVALID full comparison: 12/16 measured arms |
| Wicked moving-camera Shadows | — | — | — | — | INVALID full comparison: 13/16 measured arms |

The ten main series recorded 160 arms; 153 meet strict instrumentation criteria.
This does not make all 153 suitable for causal conclusions: caps, quality drift
and interface differences remain separate gates. Wicked's invalid arms contain
zero measured successful Presents or missing native recorder data. Their unpaired
descriptive medians and raw data are retained, never promoted to a complete series.
The extra fixed-step Diligent Compute series is INVALID for causal timing: native
drift exceeds 56% and different frame counts traverse different simulation ages.
The real-time rerun is the main Compute row above.

Cauldron's native-supported Device8/10 and bgfx's Device6–14 probes receive a
different HRESULT through the frontend. Their strict final compatibility verdict
is UNSUPPORTED even when readbacks render. The negative timings remain useful
diagnostics. Mini's cap cannot establish the requested 0.20 ms/2% CPU gate.
No measured external mode has an admitted clear rewrite, so no net engine
acceleration is attributed to ARC2. Counters named rejected_actions count disabled
or nonredundant clear admission attempts; they are not image-quality rejections.

Per-run p50/p95/p99 intervals, Present-call envelopes, thermal samples, hashes,
retained IR access counts and raw CSVs are in the versioned matrix ZIPs and
[test matrix](ARC2_TEST_MATRIX.md). Total CPU preparation, full GPU span and
physical display FPS remain unavailable in these external runs. Missing values
are not zero. Pure passthrough injects no GPU command; its CPU recording costs
still delay work submission.

## Earlier frozen-frontend performance result

Diligent GLTFViewer, static DamagedHelmet, frontend `324081d` family: two
counterbalanced `A-B-C-D-D-C-B-A` rounds, four runs per mode; each excludes ten
seconds of warmup and measures twelve seconds using parent QPC boundaries.
Capture/readback is disabled during timing. Raw data is application Present-return
cadence, **not display FPS**, GPU execution time, or isolated CPU processing time.

| Mode | Median successful Present Hz across runs | Change from native |
|---|---:|---:|
| Native | 2013.21 | baseline |
| Passthrough | 1720.11 | -14.56% |
| Observe | 1198.60 | -40.46% |
| Optimize | 1227.92 | -39.01% |

Verdict for this measured configuration: **NEGATIVE**. It is a lightweight static
scene, and graph capture/analysis overhead is material. No external rewrite or
net acceleration is demonstrated by these data. P50 Present intervals were about
0.3536 / 0.4330 / 0.4559 / 0.4474 ms respectively; full per-run distributions and
raw frames remain the authority. Thermal/order drift exists and is not discarded.
Do not treat a capped Present rate as proof of a CPU-overhead gate.

## Retention fix: separate version

`3c72ce9` replaces per-event vector front erasure with constant-time deque
retirement and shares immutable work records between active lists, retained
history and observer snapshots. `014781d` adds a separate diagnostic build;
normal frontend metering is compiled out.

At history capacity 4,096, the bounded CPU-only probe changed saturated iteration
cost from about 0.030–0.031 ms to 0.0025–0.0027 ms across three runs. This measures
the IR subsystem, not game-frame speed.

A subsequent Diligent Compute A-B-B-A check on DLL `e8b3917c...` measured native
2059.18 / 1769.84 Present Hz and passthrough 1723.35 / 1658.94 Hz. Median penalty
was 11.67%, compared with the old version's 77.73% penalty in its full matrix.
This is recovery of ARC's own overhead, **not net acceleration over native**.
The new bracket also has 14% native drift; final counterbalanced repeats remain
necessary and are not pooled with the older binary's measurements.

## Image evidence so far

* Diligent Helmet: 30/30 native/passthrough PNG pairs exactly identical; clean exit
  after moving diagnostic dump before host shutdown.
* Diligent compute particles with fixed simulation timestep: 30 aligned A-B-A
  frames, B error within the measured native-to-native envelope on all compared
  global, tile and peak metrics. Residual nondeterminism remains documented.
* Wicked early captures had substantial native drift; they are not quality
  admission evidence. Seed/temporal controls and matched sequences are being
  verified before any final quality claim.
* Cauldron and other startup screenshots establish that real scenes render;
  startup alone does not establish image equivalence or performance.

## Independent boundary-cost diagnostic

A separate metered DLL (`5c0a2b18...`, never used in the normal performance matrix)
was run on static Cauldron Brixelizer for 22 seconds with a ten-second startup
exclusion. Its 219 measured Present intervals cover 11.991 seconds. Aggregate
interceptor wall cost was 46.894 ms per Present, with interval p50/p95/p99 of
45.657 / 53.702 / 61.538 ms and 61,600 instrumented calls per Present. No meter
frame loss or site overflow was reported. This sums contributions across threads,
includes instrumentation disturbance, and is **not critical-path CPU frametime**.
The forwarded native-call envelope averaged 3.132 ms per Present; it too is a
boundary sum, not total native CPU preparation or GPU time.

Whole-session site totals (including startup) rank DrawIndexedInstanced at
4.934 s, IASetVertexBuffers at 4.796 s, SetGraphicsRootConstantBufferView at
2.726 s and IASetIndexBuffer at 2.187 s. These localize costs to API boundaries;
they do not separately measure allocation, lock waiting or ledger scans. The next
targeted experiment is immutable pipeline-state sharing plus an indexed GPU-VA
ledger, preserving alias ambiguity and descriptor generations, measured one
change at a time. See `cpu-diagnostic-cauldron/summary.json` and raw meter records.

## Same-fixture legacy comparison

The final compute fixture ran successfully through native, legacy and ARC2 paths
with identical expected GPU output. Legacy retained three nodes, all marked
unresolved, six access edges and two graph errors. ARC2 retained five work records
(including barriers), two known and fourteen symbolic accesses, no unknown access,
one shader identity and one root signature. Both dispatch root-constant values
(17 and 49) and typed heap intervals are retained in ARC2. This is better recorded
binding evidence; symbolic intervals are **not** exact executed descriptor sets.
The two schemas count different things, so no percentage reduction in unknowns
or new dependency-closure claim follows. Raw paired captures are in
`bindings-final/`, with the reproducible comparison script.

## Safety and scope

Unknown shader accesses, symbolic descriptor intervals and native-supported but
unwrapped interfaces remain explicit limitations. `Submission.complete` is not
full dependency closure. The ResourceGraph bridge is read-only and partial; it
must not authorize residency changes, culling or reuse by itself. See
[IR](ARC2_IR.md), [migration](ARC2_MIGRATION.md) and
[test matrix](ARC2_TEST_MATRIX.md). The full perceptual action controller, VRS and
shader transform portfolio have not been declared migrated by the clear rule.

CPU-to-GPU research has not been extended in this run. Present cadence alone does
not establish how much CPU work lies outside the graphics boundary, so no new
CPU-offload benefit is inferred from these measurements.
