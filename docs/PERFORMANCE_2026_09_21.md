# ARC performance implementation ledger

Approved five-block plan, 2026-09-21. No subagents; no commercial games.
Baseline: commit 87af7e0d4da07fb12f19da719f86c43c12d4edee, dev05 DLL
SHA256 b0da8cd711b1d72c8670a83dcfba7e1014bb1ddde99e4716e180691b61921bea.
Evidence root: `C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer`.

## Acceptance and immutable constraints

Native resolution, no frame generation/upscaling. Maximize complete-session FPS.
Balanced SSIM/mean/p99/worst: .98/.01/.04/.15; Aggressive: .94/.03/.12/.25.
Temporal p99 limits .02/.06; unknown alignment never grants acceptance.
Original execution, GPU proof, rollback and 600-frame evidence expiry remain mandatory.
New GUI sessions Aggressive; legacy configurations Balanced.
Net gain must exceed max(.1ms, 2%, measured noise). No concealed slow frames.

## Work

- [ ] Preserve baseline and add frame/CPU/wait/worker timeline; bounded O/N/auto diagnosis.
- [ ] Persistent bounded critic, packed-image IPC, exact striped metrics, background scheduling.
- [ ] Separate incumbent/candidate lifetimes; retained-policy analysis; evidence-age and inconclusive handling.
- [ ] Stable shader/profile lineage and cost-weighted refusal report.
- [ ] Shader local memory/CFG/coordinates/FP16, pure multiple outputs, group-coherent transform.
- [ ] Per-capability adaptive density/mip/sample search; hardware-specific proposal cache.
- [ ] Compact current-input spatial prepass and same-input scratch sensitivity probes.
- [ ] Per-transform learning, hysteresis, temporal critic, bounded resources/evidence.
- [ ] Dynamic routes, regressions, O/N/V/B/A matrix (three orders), >=25 matched frames per route.
- [ ] Same DLL Wicked 60s cases, overlay comparison, verified package, commits/push/report.

## Validation protocol

O/N/V/B/A orders: ONVBA, ABVNO, BOAVN. Initialization <=120s; measurement 60s;
process bound 210s. Keep initialization, adaptation, diagnostics and steady operation
separate in reporting. Thermal/power differences are not ARC gains. Preserve prior
package and archives. Report unresolved causes and unsupported transforms explicitly.

## Checkpoints

- Implementation started from a clean working tree. Approved plan is the specification;
  this ledger tracks actual completion, not promised capabilities.

- Initial bounded diagnostics completed: `perf-cpu-{original,neutral,automatic}-01`.
  No >50ms instrumented steady-scene scope reproduced. XInput maxima 4.95/4.59/7.89ms;
  synchronous frame serialization/write maxima 3.81/8.42/7.30ms. WPR could not enable
  system performance policy (0xc5585011); system scheduling cause remains unknown.
- Moved frame serialization/IO to bounded SPSC worker, same host for every mode.
  Order, overflow and failure tests pass; lost telemetry invalidates a run.
- Striped quality metrics match independent full-frame oracle within 1e-7, including
  11x11, partial tiles and 1080p. Resident worker protocol/shared-memory and temporal
  flicker tests pass. GPU integration is still under verification; not yet accepted.
