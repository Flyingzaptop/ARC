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
