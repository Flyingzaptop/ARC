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
