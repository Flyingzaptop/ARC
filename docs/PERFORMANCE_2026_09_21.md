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

- Confirmed NumPy/OpenBLAS defaulted to 16 threads despite OpenCV's limit. Setting
  numerical-library thread environment before imports reduced import private commit
  from ~529.5MiB to ~46.1MiB. No game affinity was changed.
- Added persistent job-bounded critic, named packed-image mappings, O/B/O/B/O GPU
  proof, local/temporal quality limits and expiry measured from captured frames.
  Native fused metric reductions match the independent oracle within 1e-7.
- `perf-resident-expanded-05` is FUNCTIONAL, not a final performance comparison.
  Trial 7 passed live quality and local A/B/A (12.327 -> 10.764ms); evidence age 535
  frames leaves very little useful hold time. Critic/scheduling overhead still needs
  work. Do not claim full-session gain from this trial.
- Added constant/private array bounds, canonical group-coordinate expressions,
  scalar output masks and additional pure FP operations. Native fixture (including
  DLL binding/rollback paths) passed in `perf-expanded-native-02`.
  Captured formerly declined ~0.303ms and ~0.148ms GI shader classes assemble/validate
  with contract 7. Shared-memory/structured-state algorithms are still declined.
- Remaining substantial work: group-coherent path, wider adaptive rates/sample
  budgets, compact spatial prepass, same-input sensitivity probes, final matrix and
  portable release. No final performance acceptance yet.

## Updated operating conditions / deadline

User reset cooling and specified GPU TDP maximum30W. All subsequent performance
series must be grouped by observed power regime, separately from the115-128W runs.
At19:06 UTC on21Sep, idle NVML still reported enforced115W (draw11.92W,35C), so
record both requested30W and actual under-load samples; do not change fan/power.
User authorized continued autonomous optimization until04:30 Europe/Warsaw on
22Sep2026 (02:30UTC). Continue useful optimization if the approved work finishes early.
Reserve the final period for comparative runs, package verification and report.

## Spatial and compute integration

See PERFORMANCE_IMPLEMENTATION_NOTES.md for current verified scope. GPU sparse
probes and learned-map oracle pass; group/shared and normalized sample CPU/GPU
oracles pass in perf-samples-native-13. CONTRACT_12 keeps old variants incompatible.
Functional learned-spatial-08 passed some image checks, but no accepted speed gain;
its whole-session FPS is not a final comparison. Warmup of map hysteresis must be
included in full session cost. Final matrix remains outstanding.
