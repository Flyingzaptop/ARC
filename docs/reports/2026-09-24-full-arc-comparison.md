# Full ARC: Wicked resident consumer + generic GPU optimizer, 2026-09-24

## Outcome

The resident CPU-to-GPU draw preparation remains beneficial with the generic ARC DLL,
automatic target-feedback controller, analysis and control costs enabled. This is
**source-assisted Wicked integration**, not automatic extraction or universal game support.

Wicked successful-Present cadence: **40.83 / 45.82 Hz original versus 76.30 / 73.38 Hz
full ARC**. Mean of the two run rates: **43.32 → 74.84 Hz (+72.7%)**. Both directions
show a gain. CPU processing-through-submission: **23.14 → 13.36 ms (-42.3%)** when
averaging the two run means. These are not displayed FPS measurements.

Cauldron: **79.33 / 73.89 Hz original versus 74.80 / 75.25 Hz full ARC**. There is
**no repeatable acceleration**. CPU offload is unsupported on this host.

The generic GPU optimizer is loaded and attempts discovery on both hosts, but did
not apply a nonzero policy in these sessions. Completed cost captures contain
capacity declines and are rejected by the controller. This is **connected but unable
to obtain usable discovery evidence**, not evidence that simplification is unnecessary.
The workload result is not a claim that every ARC optimization subsystem is effective.

## Changes

- `host_cpu_offload` is an opt-in field on the existing automatic session; absent is false.
  Permission is revoked immediately on session cancellation/stop. Older DLLs missing
  the capability exports cannot activate the adapter.
- Wicked-specific resources/admission live in `src/adapters/wicked/resident_draw_queue.hpp`.
  Its preexisting slots retire on the device's existing fences. Stop does not destroy
  in-flight buffers; device teardown frees them after the existing device-owner wait.
- Existing visible IDs, actual sort bits and resident instance data feed GPU key creation,
  sorting and pointer packing. Existing opaque draw consumers read the GPU buffer directly.
  Scene simulation, world matrices, visibility and other CPU consumers remain unchanged.
- Own shader creation is excluded from candidate discovery. Own sorting dispatches are
  excluded from game-pass cost selection while normal binding/state tracking continues.
  Their costs remain inside the measured whole frame and a separate GPU range.
- Wicked and Cauldron GPU timings carry the source frame ID through buffered retirement.
  Cauldron's timing getter now returns the just-collected slot consistently with its span.
- Added CPU-through-submission and Present intervals, counters, package comparison runner,
  unsmoothed graphs and source-frame join tests. Removed the obsolete manifest claim that
  this runner requested a 30 W limit.

## Method

A-B-B-A per renderer, fixed target **120 FPS**, existing target-feedback controller,
Aggressive profile, no new PID. A uses the same measured host in original execution
with no injected DLL. B loads full generic ARC plus the permitted Wicked adapter.
The legacy engine-aware quality presets remain deliberately off; the manifest distinguishes
this from the generic optimizer and resident adapter.

Wicked: 20 seconds warmup, 20 seconds measurement, Instances animation aligned to
measurement start, fixed camera, 1920×1080. Objects move and emissive values animate.
Cauldron: primary existing periodic route, start on its first complete-cycle boundary
after 100 seconds, at most 120 seconds, then 20 seconds measurement. Actual initialization
102.27–105.53 seconds, all windows phase-comparable and starting at pose 0. Actual window
surface is **1920×1055**, rendered 1:1, rather than the requested 1920×1080 outer size.
The same actual size is used in A/B. No resolution scaling, DLSS/FSR/FG, VSync or limiter.
Cauldron follows the same frame-indexed route; fixed-time windows cover different final
partial cycles when rates differ. They are not matched-frame image comparisons.

CPU time includes waits occurring between CPU frame entry and submission completion.
Present Hz is `1000 / mean(successful Present-return intervals)`; no display/compositor
tracing was performed. GPU spans are joined by source frame ID, not readout frame.
Overlapping queue intervals are not summed. One Wicked and three Cauldron tail GPU
samples per run have not retired by shutdown and remain missing, never replaced by zero.
No slow frames are removed; no plot smoothing or vertical clipping is applied.

No power plan, cap or clocks were changed. The user's Performance configuration was
retained; the Windows base-plan query returned Balanced, which does not identify the
OEM performance setting. The OEM setting was not independently reverified. Raw sensor
logs are included. Wicked B draws more power and boosts GPU clocks with its changed load;
the observed smaller GPU span cannot be attributed solely to reduced GPU calculations.
Cauldron reached **87 °C**, with graphics clocks falling from about 1839 MHz in A1 to
1721/1718 MHz in B2/A2. Actual power and clocks are not identical despite unchanged settings;
small Cauldron deltas are consequently confounded. No old 30 W experiments were repeated.

## Per-run statistics

All values below are milliseconds except Present Hz. Full CSV/JSON retain every sample.

### wicked

| Run | Metric | Mean | Median | p95 | p99 |
|---|---|---:|---:|---:|---:|
| A1 | CPU incl. waits | 24.471 | 22.195 | 35.191 | 56.753 |
| A1 | GPU graphics span | 6.336 | 6.180 | 8.169 | 13.200 |
| A1 | Present interval | 24.493 | 22.223 | 35.236 | 56.764 |
| A1 | Present Hz | **40.83** | — | — | — |
| B1 | CPU incl. waits | 13.092 | 12.755 | 15.282 | 18.807 |
| B1 | GPU graphics span | 4.922 | 4.932 | 5.177 | 5.855 |
| B1 | Present interval | 13.106 | 12.770 | 15.321 | 18.813 |
| B1 | Present Hz | **76.30** | — | — | — |
| B2 | CPU incl. waits | 13.621 | 12.903 | 16.039 | 26.623 |
| B2 | GPU graphics span | 5.296 | 5.283 | 5.567 | 6.590 |
| B2 | Present interval | 13.628 | 12.910 | 16.070 | 26.672 |
| B2 | Present Hz | **73.38** | — | — | — |
| A2 | CPU incl. waits | 21.816 | 21.434 | 25.161 | 27.302 |
| A2 | GPU graphics span | 6.327 | 6.400 | 6.809 | 7.925 |
| A2 | Present interval | 21.826 | 21.442 | 25.151 | 27.320 |
| A2 | Present Hz | **45.82** | — | — | — |

[CPU plot](../evidence/full-arc-20260924/wicked-cpu_ms.svg) · [GPU plot](../evidence/full-arc-20260924/wicked-gpu_ms.svg) · [Present Hz plot](../evidence/full-arc-20260924/wicked-present_rate_hz.svg)

### cauldron

| Run | Metric | Mean | Median | p95 | p99 |
|---|---|---:|---:|---:|---:|
| A1 | CPU incl. waits | 12.256 | 12.240 | 14.261 | 16.028 |
| A1 | GPU graphics span | 12.591 | 12.574 | 13.104 | 13.629 |
| A1 | Present interval | 12.606 | 12.561 | 14.681 | 16.696 |
| A1 | Present Hz | **79.33** | — | — | — |
| B1 | CPU incl. waits | 13.011 | 12.994 | 15.283 | 17.107 |
| B1 | GPU graphics span | 13.349 | 13.348 | 13.865 | 14.247 |
| B1 | Present interval | 13.368 | 13.296 | 15.870 | 17.553 |
| B1 | Present Hz | **74.80** | — | — | — |
| B2 | CPU incl. waits | 12.974 | 12.934 | 14.868 | 16.847 |
| B2 | GPU graphics span | 13.271 | 13.261 | 13.878 | 14.319 |
| B2 | Present interval | 13.289 | 13.229 | 15.268 | 17.346 |
| B2 | Present Hz | **75.25** | — | — | — |
| A2 | CPU incl. waits | 13.123 | 13.118 | 15.348 | 17.060 |
| A2 | GPU graphics span | 13.511 | 13.488 | 14.092 | 14.592 |
| A2 | Present interval | 13.533 | 13.469 | 15.869 | 17.571 |
| A2 | Present Hz | **73.89** | — | — | — |

[CPU plot](../evidence/full-arc-20260924/cauldron-cpu_ms.svg) · [GPU plot](../evidence/full-arc-20260924/cauldron-gpu_ms.svg) · [Present Hz plot](../evidence/full-arc-20260924/cauldron-present_rate_hz.svg)

## Work removed, costs and correctness

Wicked B eliminates **130,688 item visits per frame in each of build, sort and pack**
(two opaque passes over 65,344 visible instances). These are three stages over the
same objects, not 392,064 distinct objects. Counts stay at this level during measurement.
GPU preparation adds **0.502 / 0.512 ms**. Its CPU input preparation averages
**0.485 / 0.478 ms** and command recording **0.275 / 0.272 ms**, already included in
full-frame cost. There is no intermediate algorithm-result readback, no new CPU wait,
and no additional frames in flight. The input transfer is 522,752 bytes/frame and
persistent sort/pointer buffers occupy about 2.5 MiB across the two existing slots.

The overall gain survives all enabled ARC costs. The standalone total cost of ARC
versus this exact host with only resident offload was not separately measured here;
the previous experiment was not subtracted as though conditions were identical.

One final oracle/stop run validated **33,782,848 records over 517 frames**, zero exact
ID/depth errors and nonempty coverage on every checked frame. After stop, permission
and skipped-item counters were zero. This covers the admitted homogeneous opaque scene,
not arbitrary materials, transparency or arbitrary game semantics. No GPU simplification
policy was applied during the performance sessions; quality knobs stayed original.
The oracle is same-frame prepass ID/depth, not a general perceptual RGB critic.

Earlier short attempts are retained as diagnostics: one ended in initialization;
another optional camera route produced an empty image and was rejected. The successful
fixed-camera, moving-object oracle is the only correctness result counted.

## Cauldron ablation and remaining blocker

The one requested ablation retained the same DLL but disabled the automatic controller/
discovery (`compute-off`, no automatic config). It achieved **76.00 Present Hz**,
CPU **12.826 ms**, GPU **13.143 ms**, Present interval **13.157 ms**. Swapchain waiting
averaged **6.305 ms**, allocator acquisition/wait **0.124 ms**, submission **0.157 ms**.
This is slightly faster than B, but lies within the original A1/A2 spread. It does not
prove a precise controller overhead or establish a repeatable regression.

The deterministic blocker is discovery: Cauldron captures reach **100,000 observed
events**, mark a capacity decline, and are rejected. Wicked profiles also have capacity
declines (different cause not yet individually classified). Thus GPU optimization is
connected but cannot make useful decisions on this measured run, and Cauldron stays
GPU-bound at roughly **13.3 ms** against an 8.33 ms target. No supported CPU offload
chain exists there. On Wicked the remaining `Application Update` averages **7.90–8.42 ms**
inside total CPU 13.09–13.62 ms, while GPU is 4.92–5.30 ms; that is the next CPU region
to inspect, not permission to transfer it without proving its consumers/dependencies.

The next bounded experiment: classify each GPU capture rejection separately, adapt
capture length to the existing event budget (rather than dropping validity checks),
and recheck whether the controller can obtain complete costs and activate an applicable
existing transform. For Wicked, distinguish copy-list exclusion from real graphics/
compute overflow before changing admission. No universal extractor or new optimizer
was added to disguise this failure.

## Delivery and reproduction

- [Interactive-free report with all six plots](../evidence/full-arc-20260924/index.html).
- [Raw measurements archive](../evidence/full-arc-20260924/raw-measurements.zip), including
  failed diagnostics, final oracle, all A/B windows, decision logs, profiles and ablation.
- [Binary hashes](../evidence/full-arc-20260924/package-manifest.json).
- Ready local binaries: `C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-20260924/package`.
- One command: `./scripts/compare-full-arc.ps1 -Output C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-repeat`.
- Source patches and build prerequisites: [benchmark README](../../benchmarks/full-arc/README.md).

Validation: Release DLL + Wicked + Cauldron builds succeeded; two source-frame/outlier
analysis tests passed; both integration patches applied to captured bases and matched
compiled sources; final integrated GPU oracle and stop test passed. No historical full
matrix or commercial game was run. All final performance runs and the final oracle exited normally. The original Wicked executable was restored; the integrated build is preserved in the package.
