# Autonomous product delivery

Approved 2026-09-20. One final delivery, no subagents, no commercial-game tests.
The source baseline is b7a8d73 on codex_den/generic-dx12-runtime.

## Acceptance ledger

- [ ] Automatic acceptance/retention with complete cost and GPU execution evidence.
- [ ] Measured multi-pass policy selection and combined-policy quality validation.
- [ ] Screen importance, balanced/center profiles, conservative unknown mapping.
- [ ] Useful bounded temporal reuse with dependency invalidation and measured cost.
- [ ] Exact CPU graphics-state/descriptor redundancy elimination with net savings.
- [ ] Launcher, target control, rollback, self-contained dependency package.
- [ ] Same DLL: dynamic Cauldron/Wicked, three paired repetitions and regression tests.

Neither scaffolding nor all-candidates-declined counts as product completion.
Preserve image limits (SSIM .98, linear mean .01, tile p99 .04), native resolution,
steady own CPU .2ms and GPU .25ms budgets. Include discovery in whole-session FPS.
Runs are capped at 60s, with >=35s active measurement after scene readiness.
Report no-DLL, loaded/unchanged, and automatic execution separately.

CPU scope is graphic API/driver work and ARC overhead; internal simulation,
animation and geometry algorithms are not covered without an engine adapter.
Speculative future rays are deferred, not substituted with shader precompilation.
Twofold FPS is a target, not a guaranteed outcome or a changed quality threshold.

## Work log

- Confirmed existing runtime uses a single selected pipeline, ordinal synthetic
  candidate gains, incomplete GPU overhead and CPU submission-only provenance.
  These gates must not be removed or replaced with zero to manufacture acceptance.
- b5725ee: measured top-eight compute catalog and atomic complete policy bundles;
  automatic candidates now use observed work cost instead of ordinal invented
  gains. 53 non-GPU tests and injected exact-pixel compute regression passed.
- Dynamic discovery runs product-catalog-auto-01/02 complete 3000 frames in
  51.07/50.33s at 69.55/69.87 FPS. Four image trials pass in each, none retained.
  These are NOT a performance gain or product acceptance. Costs remain above
  budget and complete GPU cost/provenance remain open. Preparing only the trial
  targets and limiting detailed hook timing to timing windows did not establish
  a significant whole-session improvement.
- Exact native state-cache prototype: PSO, viewport/scissor, topology, blend,
  stencil. Command-owned shared state with bounded thread-local lifetime lookups
  avoids adding the global optimizer lock to every repeated setter. Reset,
  indirect/opaque work, object generations and CPU mode changes invalidate it.
  Native raster/VRS pixel equality and debug-layer regression pass. The first
  lock-based version was slower in the CPU recording diagnostic and was replaced.
  product-cpu-native-release-02 records 3.05--3.35ms enabled vs 3.81--4.77ms on the
  warm later disabled runs; this is API recording only, not game FPS evidence.
- Native test setup exposed D3D12SDKLayers 10.0.26100.5660 crashing on the
  documented optional OMSetBlendFactor(nullptr), reproduced WITHOUT loading ARC.
  The fixture now passes the equivalent explicit {1,1,1,1} array. No production
  API behavior was changed to hide that SDK issue.
- Added explicitly bounded GPU calibration: neutral controlled shader and original
  shader execute against the same admitted read-only inputs. The extra original
  dispatch is GPU-predicated and disappears on cached replay after calibration.
  Native tests verify every output before/during/after calibration and actual
  copied bytes for active/zero predicate values. Timestamp readbacks retain the
  submitted epoch and pipeline identity across control-pool reuse.
- product-calibration-auto-01: 3000 frames, 80.086 FPS, 43.13s, zero ARC faults.
  Seven calibration windows each issued and collected six samples; dispatch,
  guard, neutralization and upload component estimates span .116--.318ms/frame.
  The adjacent no-DLL baseline measured 80.093 FPS (42.25s). This is no demonstrated
  FPS gain. Full GPU-cost/final-frame-provenance gating remains closed; CPU
  candidate-window cost is still 1.06--1.56ms/frame and exceeds the .2ms target.
- CPU view caching now compares full native SRV/UAV/CBV descriptors (not the
  deliberately partial semantic ledger), including resource/heap generations.
  Descriptor copies and sampler-feedback UAV writes invalidate the affected
  cache entries. Native source-copy/restore and per-pixel tests pass.
- product-cpu-renderer-off/on-01: 81.27 / 80.37 FPS; median CPU preparation
  3.543 / 3.594ms. This single pair does not support enabling the CPU cache on
  Cauldron. The on run eliminated 40,583 setters and 130,546 view creations;
  eliminating calls alone is not an acceptance criterion.
- The same session controller now trials CPU-only exact-state changes before
  GPU profiling, requires two bracketed confirmations, and distinguishes their
  structural equivalence proof from image measurements. It does not invent SSIM
  values. GPU candidates cannot use this exemption. Automatic mode owns policy
  and diagnostic requests exclusively; affinity experiments are set to normal.
- Automatic shader preparation is scheduled by measured expense instead of
  compiling every observed pipeline at startup. Descriptor heap lookup hints
  retain numeric IDs and revalidate current ranges, including copy/move/reuse.
- Product-exclusive-auto-01: 3000 frames, 80.835 FPS, 43.09s, no ARC faults.
  CPU trials correctly rejected inconsistent gains; observed CPU cost .927--.936ms.
  Three shaders prepared, 39 declined, one unrequested low-cost pipeline retained.
- Fixed cadence comparison bias: candidate CPU-cost sampling is now separate
  from candidate FPS timing, so incumbent/candidate timings use equal metering.
  An empty-hook diagnostic measured ~26ns reported own time and ~64ns total
  instrumentation per call. No cost budget has been relaxed or debiased by fiat.
- Added target-FPS update export and reject further degradation if the updated
  target is already met. Whole-policy gains no longer sum unrelated increments.
- Calibration is disabled inside application statistics queries; the native test
  verifies the exact original CS invocation count. The latest Release build,
  53 non-GPU tests and injected query/calibration/descriptor pixel oracle pass.
