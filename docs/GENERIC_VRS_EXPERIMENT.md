# Experimental native command substitution

This is a development mechanism, not an accepted automatic game optimizer.
The at-least-2x real-game FPS goal remains unverified.

## Mechanism

`--vrs-2x2 PID DLL` enables a maximum 60-second experiment. Newly recorded direct
command lists can have an alternative recording with per-draw 2x2 VRS. The
original application recording is always preserved. Submission chooses the
alternative only when command coverage and pipeline eligibility are known.
`--vrs-off PID DLL` selects original lists again, including already-closed cached
lists; it does not rely on the game resetting them. Already-submitted work finishes
normally. The experiment automatically expires even if the caller disappears.

The implementation covers base graphics commands, native render passes, enhanced
barriers, and known raster ExecuteIndirect signatures. Unknown signatures, bundles,
unsupported newer operations, unknown pipeline state, MSAA above four samples,
and target-independent rasterization cannot authorize an override. Graphics PSOs
created with either the legacy descriptor or a bounded pipeline state stream are
recognized. This eligibility establishes API compatibility, not image quality or
absence of semantic shader side effects. Live automatic admission is not established.

Allocator/list storage is retained behind queue fences; a failed Signal stops new
experiments and retains possibly in-flight storage. Completed allocators can be
reused. Prior non-raster or unsupported recordings get a metadata-only generation
before becoming eligible again, avoiding repeated construction of discarded lists.

## Measurements

`--measure PID DLL NEW_JSON [SECONDS]` captures CPU Present-return intervals for
1–60 seconds (default 60). It records raw intervals, aggregate cadence, nearest-rank
percentiles and the inverse mean of the slowest 1% of intervals. It does not measure
GPU busy time or verify displayed FPS. Occlusion/failure, timestamp errors, exhausted
sample capacity, timeout and experimental-mode changes invalidate a window.

`--lean PID DLL` disables detailed object/descriptor/work tracking for the remainder
of the process while retaining image capture, timing and experimental command
substitution. Previously collected object metadata is explicitly marked non-current;
graph capture is refused. This diagnostic separates observer cost from mutation cost.
Removing ARC's own overhead must not be presented as a gain over the unmodified game.

## Verified so far

- 47 non-GPU CTest tests pass, including cadence edge cases.
- Native direct and indirect draw tests exercise both legacy PSOs and pipeline
  streams/render passes. Pixel output changes under VRS. Disabling VRS restores
  every pixel exactly when the same cached original command list is submitted.
- Unsupported-operation fallback reproduces original pixels. A later supported
  recording can requalify; it is not permanently excluded by an earlier gap.
- Bodycam actually executed substituted commands without Present errors. In the
  first run, 10-second cadence windows were 23.56 FPS baseline, 15.20 under the
  experiment and 16.94 after restore. Camera positions differed and baseline drift
  was substantial; this is a rejected experiment, not a speedup.
- A subsequent 33.38-second four-phase diagnostic recorded 15.91 FPS with detailed
  observation, 29.85 in lean mode, 23.39 with lean+VRS and 30.21 after disabling VRS.
  These short, uncontrolled-input windows establish a problem to investigate,
  not an accepted performance/quality result. That binary predates the latest
  metadata-only qualification and raster-indirect support.

Local raw data: `ARC-Hardening-GPU/bodycam-vrs-20260919-151645` and
`ARC-Hardening-GPU/bodycam-lean-20260919-152310`. Each includes the actual tested DLL.

Next validation must measure the current implementation, separate observer cost,
and connect genuinely comparable image evidence to admission/rollback. VRS only
changes raster pixel shading; compute lighting, ray work and geometry workload
reduction remain separate requirements. No result here proves the user's 2x goal.
