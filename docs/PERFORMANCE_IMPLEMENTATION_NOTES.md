# Implementation continuation notes (not completion claims)

Updated 21 Sep2026, approximately21:00UTC. Active work in ARC-perceptual,
branch codex_den/generic-dx12-runtime. No agents/commercial games. Deadline
22Sep04:30Warsaw (02:30UTC). Observed loaded GPU remains115W despite requested30W;
keep actual power cohorts separate and never label these as30W results.

## Implemented since daec4d5

- ABI CONTRACT_12 /144-byte ControlValue: compact full-tile PSO, per-recipe
  256-bin empirical error tables, >=3distinct candidate frames plus neutral noise,
 8-observation hysteresis and immediate original fallback for unknown inputs.
- Sparse raw-buffer shadow probes on identical inputs, <=1/16tiles, >=31frame
 spacing, one in-flight/device,16MiB pair. Per-output masks/64bit epochs, fence
 readback, maximum normalized error across outputs. App outputs untouched.
-8control slots per device pool; additional devices fail closed to keep combined
 spatial/scratch default heaps below256MiB. Allocation counters include scratch.
- Dynamic4x rates and half-step mips through+4; group-coherent shared-memory path
 with convergent acyclic barriers and bounded local pointers; group-uniform
 original fallback on incomplete coarse regions. No shared spatial probes yet.
- One fixed-count independent normalized sample-mean loop may use75/50/25percent.
 Recurrent, dynamic, unnormalized and RayQuery loops are not eligible. Neutral and
 rollback preserve original arithmetic. Sample reduction currently global/pass,
 not combined with the spatial actuator.
- Quality worker scheduling priority and overlapping renewal timing only under
 still-valid incumbent proof. Critic CPU memory remains~600MiB (512target unmet).
- Compiler child now owned by kill-on-close Job Object.

## Evidence

78 CTests passed (before final group/sample additions); compact learned-map GPU
oracle passed, including immediate restoration on new detail. Sparse raw probe
GPU tests passed (multiple outputs, partial tiles, mask mismatch, stale epochs,
untouched app outputs). Native shader fixture perf-samples-native-13 passed all
pixel CPU oracles including shared full/partial groups and normalized samples.
Actual formerly-rejected shader367 now assembles/validates; this is NOT a speed
or quality result. Raw source IR stays outside the repository/package.

perf-learned-spatial-07 had no accepted effects: image probe map warmup was too
short.40frame warmup fixes it. perf-learned-spatial-08 has live quality passes but
no useful timing gains. B timing warmup now40frames as well; needs fresh rerun.
Previous expanded-05 local gain is not a new full-session performance result.

## Still required

- Performance proposal cache keyed by device/driver/render/profile, fresh proof.
- Further measured overhead reduction; no repeatable new full-session gain yet.
- Verify phase-locked host/init bounds and holdout route; add independent object/
 light/CPU-command dynamics if feasible. Same host for O/N/V/B/A.
- Final three-order15run matrix,25matchedframes/route, Wicked2x60s, overlay, package,
 full tests, git/push and honest report by deadline. Preserve negative results.


## Later checkpoint (approximately21:45UTC)

Commits cf74a1f (spatial/group/sample) and cb5ca86 (phase/holdout camera) pushed.
Uncommitted: hardware-keyed performance-proposal cache (unit cold/warm/corrupt
PASS), candidate deduplication, cost/fraction-aware search, exact submitted-policy
activity counters, calibration descriptor-proof warmup, detailed A/B/A GPU profiles
only with --measure-costs, overlay recipe/error labeling, critic earlier temporary
array release (Python tests pass), and holdout light animation (host built).

Important diagnosis: visible Windowed original NO DLL has ~61>100ms frames/min,
~500ms waits in swapchain queue fence. Disabling nvidia-smi did NOT remove them.
Switching the SAME1920x1080 original host to SDK --fullscreen (BORDERLESS) gave
76.495FPS,0>100ms,max~18ms (perf-borderless-no-sensor-15). With normal sensors:
72.628FPS,0>100ms,max18.103ms (perf-borderless-sensor-16), all115Wcap samples.
This is a presentation-mode workaround, NOT ARC acceleration. Final matrix uses
borderless in ALL modes. Exact driver/compositor cause remains unknown.

Power really changed to30W/810MHz memory during search-cost-11 and portions of
later runs, then115W/7001MHz again. Preserve and classify hardware cohorts; mixed
runs cannot support acceptance. Do not change user's cooling/power.

Automatic borderless17 still no accepted policy (mixed115/30W). Fullpass and
spatial trials sometimes pass image quality, but timing not enough. Prior maps
had too-short hysteresis warmup: capture and B timing now40frames. Initial new
recipe warmup remains128..384frames; optimize cached-training readiness later.

Paired diagnostic19 fixed startup descriptor-proof race (first6admissions failed
before warmup):6GPU pairs now prove~0.232ms wrapper/control overhead per frame;
original DeferredLighting~3.45ms,neutral wrapped~3.62ms. Need inspect real modified
pass time: next build adds symmetric timing-before/candidate/after GPU profiles.
Do not assume wide4-store code is the bottleneck without that evidence.

Wicked runner now allows60s and bounded init; external patch applied to existing
Wicked checkout (warmup limit120,raw wall/GPU frame arrays), NOT rebuilt yet.
Matrix runner rewritten for O/N/V/B/A three specified orders, frozen binary+
critic hashes,100s-to-cycle-boundary init<=120,60s measure,210s hardlimit. Uses
frozen product-package-dev-05 for V (its Python3.13/Numpy2.2.6/OpenCV4.12 tested).
Final matrix,25matchedframes/route, Wicked,overlay/package/report still pending.

Latest source has activity API present_frame(frame,qpc),approve_policy, counters;
NOT compiled since those edits. Finish builds and relevant tests before runs.
Current GPU test19 finished. No commercial games or other intended GPU jobs.

## Pre-matrix checkpoint (21Sep23:40UTC approximate)

0523a11 committed runtime/critic/cache/scene guard and tests (not pushed yet).
One follow-up uncommitted C++ fix: scene uniform guard compares only observations
within4frames; comparing across long policy-off gaps falsely treated normal
camera motion as a cut. Pilot23 exposed this; rebuilt and full CTest now running.
Do not edit compiled sources while builds/tests/matrix run.

Native tests: perf-final-native-21 withoutDLL and22 withDLL PASS, including
sample-mean and shared-group pixel oracles. Native quality-worker test PASS:
stale reply, one restart, healthy process kept after context invalidation,
normal cancellation not charged as crash,15s timeout,1GiB Job memory limit.
Current worker uses separate failure count, drains obsolete immutable requests
without killing Python; user stop still closes it. UTF8 stdio/metadata explicit.

Critic default now keeps original images packed until needed; processes float64
warps in phases. Saved real5frame request:~438MiB peak vs~622MiB, metrics EXACTLY
match on tested capture; warm~3.57s vs~3.37s. Pure unscaled-float32 packed warp
experiment had451MiB but slower~3.93s, so NOT default. New permanent packed metric
oracle test passes (8/10bit,1080p,1e-7 tolerance). Core immediately revokes a known
bad incumbent, but retains an incumbent when a DIFFERENT candidate fails. This
fixed pilot20's incorrect hold after confirmed revalidation failure.

Pilot20 BEFORE native-TAA/animation fixture correction: repeated accepted full2x2
on pipeline132; GPU profile19~3.64ms ->1.77ms, fullwindow12.8 ->10.5..10.9ms.
Only~4% accepted-submission duty then. Shortened unnecessary global timing settles
and revalidation pre-wait; overlapping A/B/A only while old approval still valid.
Fresh pilot needed. No final whole-session performance claim yet.

Host is now rebuilt with:
- SDK native TAA jitter callback restored (old camera override skipped it).
- Object animation is a deterministic smooth10s cycle of existing clip segment;
  camera route unchanged, holdout adds stop/turn/light modulation.
- ALL initialization frames logged in initialization-frames.jsonl, measurement
  contract unchanged; single async writer handles both, no tail filtering.
- --oracle-poses:32 distinct absolute frames1200..1799, including short sequences;
 1680 measurement frames+120warmup,210s process bound,quality-only,notFPS evidence.
- --borderless uses SDK -fullscreen and retains1920x1080; Windowed queue stalls
 reproduced withoutDLL AND withoutsensors; Borderless removed~500ms waits.

Wicked rebuilt successfully (v145, MT libs in build-wicked-generic) with60s runner,
init<=120,raw wall/GPU arrays. Same main DLL must be used; do not rebuild kernel.

Final obligations still PENDING:15-run matrix, independent32-frame O/O+B+A on
both routes, Wicked Hello/Instances60s, overlay/F9/F10/resize, portable package,
report and push. Matrix runner5modes already rewritten; uses previous dev05 package
for V and its ownPython/critic. Summarizers added. Preserve all negative evidence.

CURRENT tools: CTest running in exec session95529, log build/final-ctest.log.
No owned renderer running. Previous GPU pilot23 ended. Do not start GPU runs
until CTest GPU fixtures finish. Deadline22Sep04:30Warsaw =02:30UTC. Start final
matrix preferably by00:00UTC to leave time for all other verification and report.
