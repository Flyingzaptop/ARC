# ARC implementation status

Execution ledger for `ARC_AGENT_IMPLEMENTATION_PLAN.md` (2026-09-23). Status labels describe this plan's deliverables, not ARC's overall maturity. Update this file at each milestone; do not infer performance, quality, or game support from implementation alone.

## Acceptance correction after review of `3413c2b`

Following the user's review of `8af02dc`, **M0–M3 are accepted in the declared prototype scope**. M4 remains pending acceptance. Its reproduced mixed-hit/miss cost error is corrected: profitability uses the arithmetic mean of all calls; median/IQR remain noise diagnostics. The exact 9,000 ns versus 20,800 ns regression and the existing positive selection test pass. See `docs/evidence/M4_MEAN_COST_FIX.json`. No full matrix or native execution was repeated for this arithmetic-only correction.

The earlier blockers and scoped checks remain recorded in `docs/reports/2026-09-23-m0-m4-corrections.md` and `docs/evidence/M0_M4_CORRECTIONS.json`; those historical records describe their original versions.

## Grounded base

- Repository: `C:/Users/r3d_flzp/Desktop/ARC-perceptual`; branch `codex_den/generic-dx12-runtime`; HEAD `3b56c4bdca4cb471dc513bc2f95d84f830cdcb36`. At the initial M0 snapshot, `git status` was clean and `origin/codex_den/generic-dx12-runtime` resolved to the same commit. No checkout, fetch, rebase, or branch mutation was performed.
- No repository-local `AGENTS.md`; ancestor `C:/Users/r3d_flzp/AGENTS.md` was read. Its general development conventions apply where relevant; the current user explicitly authorized Sol/Luna delegation.
- Windows x64 is available. Visual Studio 18 Build Tools (MSVC 14.50.35717) and its `vcvars64.bat` and CMake executable are installed; CMake is not on the current shell PATH. Existing `build/` is configured for x64 with the Visual Studio 18 2026 generator and contains 39 `arc-*-tests.exe` executables plus native fixture executables. These are artifacts, not a test run or proof they match current sources.
- Pinned DXC v1.9.2602.24 is available at `C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/deps/dxc-1.9.2602.24/bin/x64/` (`dxc.exe`, `dxcompiler.dll`, `dxil.dll`). No reinstall/download or baseline suite was needed. Exact DLL hash and M1 check dependency fingerprints are in `docs/evidence/M1_CHECKS.json`.

- Relevant existing evidence is summarized in `docs/reports/2026-09-22-cpu-recovery-sm66.md`, `2026-09-22-target-feedback.md`, `2026-09-22-bodycam-live-test.md`, `2026-09-22-bodycam-cpu-baseline.md`, `2026-09-22-shadow-reuse-audit.md`, and `2026-09-22-shadow-predication.md`. Reports describe earlier builds and remain scoped to their named fixtures/runs.

## Milestones

| Phase | Status | Existing paths / evidence | Last applicable check | Invalidation fingerprint | Gap and next action |
|---|---|---|---|---|---|
| M0 Grounding | complete | Base and report references above; this ledger | Read-only checkout/tool/report audit, 2026-09-23 | repo + branch + HEAD `3b56c4b`; remote ref same SHA | Exit met. Reuse this base; refresh only after source, branch, or toolchain changes. |
| M1 Contracts and bounded observation | implemented / local_correctness / native_checked; game_pending, benefit_unknown | `candidate_contract.hpp`, `discovery_budget.hpp`, `cheap_observer.hpp`; connected compute/pixel publication gates, shared worker quota/cancellation, capture quotas, cache persistence, idle observation/rearm | Focused 4 suites and owned native cases in `docs/evidence/M1_CHECKS.json`; no commercial games | Per-check source/transitive inputs, Release DLL hash, DXC hash and driver inventory in evidence JSON | M1 corrective checks pass within declared runtime coverage; acceptance remains open. M2 now supplies a separate CPU session. Combined CPU/DX12 ownership is not enabled. |

| M2 CPU discovery | implemented / local_correctness / native_checked (DBI); new ETW provider capture pending | `src/backends/cpu/`, `scripts/analyze-cpu-regions.py`, `capture-cpu-work.ps1` | Pinned Windows DynamoRIO discovers code without fixture names; bounded study records bytes/ops/live-ins. Parser and WPR schema pass | CPU source hashes + DR archive hash + fixture + OS in M234 evidence | Main EXE; bounded 64-slot activity/recency pool with re-admission and 128 rotating observers. ETW buckets are proposals, not proof. New synchronized presentation layout/overhead needs user-run elevated capture. |
| M3 CPU execution / specialization | implemented / local_correctness / native_checked for closed register-only domain; game_pending | `include/arc/cpu/`, `src/backends/cpu/client.cpp`, `decoder.hpp` | Real DBI specialization and guard fallback; GPR/flags/SIMD/MXCSR/x87/stack oracle; shared publication, stop and protection invalidation | Decoder/model/publication + DR + native fixture hashes | Native decoder supports 64-bit MOV/LEA only. No memory, calls, arbitrary function/LOD rewrite. Explicit actuators diagnostic; auto now measures original/specialize/memo/incremental paths and retains only positive within-DBI estimates; this does not establish gain over no ARC. |
| M4 Memoization / incremental recompute | implemented / local_correctness / native_checked for register results; game_pending, no demonstrated net benefit | `region_model.hpp`, same DBI executor | Exact live-in comparison, generation reset, real memo and partial dirty-node execution; original fallback; native matrix | Same CPU executable/model/fixture fingerprints | Automatic acyclic dependencies extracted from decoded instructions. Memory-backed/isolated output-region reuse and arbitrary loops remain unsupported. Current diagnostic process totals are slower than original. |
| M5 Structured GPU analysis/admission | pending | `src/backends/dx12/generic_shader_transform.cpp`, `generic_binding_state.cpp`, `generic_binding_admission.cpp`; CPU recovery report records bounded CS 6.6 static binding support | Earlier DXC/validator negatives and GPU oracle summarized in report; pinned DXC present; old checks reused only within their recorded scope | Analyzer/DXC version + shader model + resource/heap generations and declarations | Existing subset does not cover dynamic heap/bindless access or prove general completeness. Next: connect structured facts to action-specific admission; retain declines for unknown dependencies. |
| M6 Exact reuse / graph transform | pending | `include/arc/shadow_reuse.hpp`, `src/backends/dx12/generic_shadow_inputs.hpp`; shadow/predication reports | Prototype/fixture evidence only; no complete live-game exact reuse path established | M5 analyzer/admission + resource generations + queue/fence/lifetime model | Prototypes do not establish live pass reuse or a bounded graph rewrite. Next: deliver one exact live path with admission, fallback and retirement evidence. |
| M7 Approximate GPU actions | pending | `src/backends/dx12/generic_pixel_optimizer.cpp`, `generic_target_feedback.inl`; target-feedback report | Prior functional Cauldron sessions; quality evidence intentionally absent in `target_feedback` | M5 action admission + quality policy/reference/context + shader/resource generations | Controller can change bounded density/sample/mip actions, but reports `quality_verified=false`; no quality-admitted approximate result. Next: add action-specific quality evidence/admission. |
| M8 CPU/GPU provenance and advanced CPU experiments | pending | CPU trace scripts and existing GPU profiling paths are diagnostic inputs only | No simultaneous per-frame CPU/GPU critical-path evidence; reports say full generic CPUft/GPUft unavailable | M2/M3 provenance + M5 GPU admission; M7 quality evidence for approximate actions | No causal CPU-to-GPU provenance or admitted advanced pre-DX action. Next: correlate same-window evidence before selecting one bounded experiment. |
| M9 Joint planner and learning | pending | `src/backends/dx12/generic_auto_session.cpp`, `generic_target_feedback.inl` | Existing feedback behavior described in target-feedback report; bottleneck remains unknown | M1 candidate contract + available actuators + evidence schema and policy version | Existing controller is not a joint CPU/GPU planner with persistent candidate ranking/cache. Next: plan only across admitted, measurable actions. |
| M10 Package and user test kit | pending | `src/launcher/arc_launcher_main.cpp`, `scripts/package-optimizer.py`; existing package tooling | No plan-specific reproducible CPU vertical-slice package checked | Completed phase SHAs + pinned dependency hashes + package scripts/config | User flow has not been reconciled with implemented CPU study/application modes and returned-evidence kit. Next: package after M3/M4 slice, then update from actual minimal integration checks. |

## Evidence boundaries

- The 2026-09-22 Bodycam report used build `37a16c4`, not this base: mean FPS fell from 47.556 to 22.642; prepared/modified compute and pixel work and VRS modifications were all zero. It is a negative live run, not evidence against later code or a speedup claim.
- The CPU recovery report has useful owned-fixture and shader-oracle results, but explicitly says the primary pre-DX application CPU-work objective is unsolved. The later `2026-09-22-bodycam-cpu-baseline.md` records successful ETW collection and initial analysis; it does not identify object/LOD functions or a simultaneous critical path.
- The target-feedback report explicitly leaves full generic CPU/GPU critical-path values unavailable and does not certify image quality. The shadow/predication prototypes are not a complete live-game reuse path.
- Reuse historical evidence only while its source/build, dependency/tool hashes, configuration and fixture assumptions match. User-run game performance/quality and unperformed native checks remain pending.

## M1 implemented behavior and checks

- Common immutable candidate projection is actually gated in compute submission and pixel record/submission paths. Experimental approximate candidates remain `quality_verified=false`. Static contract facts do not replace runtime binding/alias/generation guards.
- Invalidated publications reject new admission; previously recorded GPU code/resources remain in existing shared owners and fence retirement. Every active-to-passive transition forgets descriptor evidence. Cached pixel lists cannot pick up nonneutral controls from an invalidated publication after resume.
- Shared queued/running job reservations enforce count and byte quotas. One background compute gate serializes heavy compiler/critic work. Stop invalidates job epochs, clears queued work, cancels owned compiler children in bounded polling, rejects late publication. Immutable compiled pixel code is republished after resume without recompilation.
- No-candidate runtime keeps Present/submission and PSO-creation observation active. Heavy profiling uses cooldown; novelty or a budgeted retry rearms discovery. Hint loss only reduces diagnostics; an observation gap advances correctness generation and invalidates dependent admission. Creation hints are not proof. Passive root/PSO bytecode and layouts now have bounded owned retention and replay; oversized/unsupported captures report drops.
- Positive/negative analysis cache keys retain code/layout/compiler identity. Capacity/budget/timeout failures are not persisted as semantic negatives. Disk bytes, high water and evictions are exposed.
- Per-producer observer rings are preallocated (64 x 64 events), drain is bounded to 512 events per collector call. Snapshot exports dropped diagnostics, novelty and correctness epoch separately.

| Check | Outcome | Evidence |
|---|---|---|
| Shared lifecycle/admission | PASS | Concurrent activate/invalidate, existing lease retirement, unknown facts, scoped correctness loss |
| Discovery quota | PASS | Job bytes/count, cancellation, capture window/event/byte bounds, backoff |
| Observer | PASS | Overflow preserves novelty, bounded fair drain, timestamps/sequence/TID/frame hints |
| Cache | PASS | Stable semantic negatives, transient retries, corruption and eviction accounting |
| User stop and resume | PASS | `m1-runtime-final`: 100 Presents/submissions, compiler killed, novelty during idle, resumed observation, healthy GPU |
| Connected compute/pixel gates | PASS | `m1-compute-native-final`, `m1-pixel-native-final`; output/rollback, dynamic proof, invalid cached pixel publication |
| Profiler stop/lifetime | PASS | `m1-profile-native-final`; original open list remains valid after stopping capture |
| No-candidate continuous mode | PASS | `m1-continuous-observer-final`: 1800 Presents, phase `observing`, deep discovery idle, zero hook failures |

Native evidence directories are under `C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/`. All tests are owned fixtures. The one longer overload run resolves continuous-controller integration and remains under the 60-second cap; other native checks are short. Full historical Cauldron/Wicked matrices were not repeated.

## Bootstrap configuration

One `DiscoveryBudget::Config` defines defaults. Startup-only overrides are read before workers; changes require process restart. Effective values and consumption appear under `discovery_budget` in runtime JSON.

| Setting | Default | Override |
|---|---:|---|
| Heavy workers / simultaneous CPU studies | 1 / 1 | Fixed until contention is measured |
| Outstanding analysis jobs / retained bytes | 8 / 64 MiB | `ARC_DISCOVERY_MAX_JOBS`, `ARC_DISCOVERY_MAX_BYTES` |
| CPU study window / events / payload | 100 ms / 100000 / 8 MiB | `ARC_DISCOVERY_CAPTURE_MS`, `ARC_DISCOVERY_CAPTURE_EVENTS`, `ARC_DISCOVERY_CAPTURE_BYTES` |
| CPU study spacing | 1000 ms | `ARC_DISCOVERY_CAPTURE_SPACING_MS` |
| GPU profiling window / events / retained payload | 10000 ms / 100000 / 8 MiB | `ARC_DISCOVERY_GPU_CAPTURE_MS`, `ARC_DISCOVERY_GPU_CAPTURE_EVENTS`, `ARC_DISCOVERY_GPU_CAPTURE_BYTES` |
| Failed-discovery retries | 4 / 8 / 16 / 30 s | Context changes cannot bypass cooldown |

The DX12 runtime still reports `cpu_capture_backend_available=false` for its own session. M2 uses a separate CPU-only DynamoRIO launch; the two runtimes are not combined. Existing GPU query/recording caps remain additional independent bounds. Own meter microcheck: about 1.35 ns disabled and 66.91 ns enabled per empty call on this host; no overall game-cost claim follows. The 0.2 ms CPU / 0.25 ms GPU goals and real-game usefulness remain unverified.

## M2–M4 supported CPU delivery

Continued from clean `02c42a8`, preserving M1 and the existing DX12 backend. No commercial games launched. See `CPU_BACKEND.md`, `CPU_DISCOVERY_CAPTURE.md`, and `docs/evidence/M234_CHECKS.json` for reproducibility and limits.

- Pinned Windows x64 DynamoRIO 11.3.0-1 is integrated into a separate CMake build. Its license and archive SHA256 are recorded; the user launcher has baseline/neutral/study/apply modes.
- Native discovery uses decoded executable structure, not function labels or fixture addresses. Supported prefixes are closed register-only MOV/LEA, maximum 64 instructions and 64 candidates. Cold entries can be displaced by late hot regions; stale translated callbacks request rediscovery instead of using a reused slot.
- Specialization, memoization and incremental recomputation execute through the actual process boundary. All output GPRs are preserved; flags, SIMD, MXCSR, x87 and stack have a native oracle. Memory-dependent regions decline; no pointer-equality reuse is claimed.
- Exact byte comparison and RX-page admission supplement code/module identity. Protection changes invalidate publications; external remote code patching is outside the supported assumptions. No aggressive live patcher is installed.
- Stop uses a shared mapped 4-byte control, with no per-region file IO or extra in-process monitor thread. Outstanding fixed storage remains alive until process exit.
- Allocation-free candidate admission fixes the observed CRT-heap crash. A separate DR client-thread startup crash was avoided by the mapped control. Trace-copy instrumentation and correct x87 save/restore were additional regression fixes.
- `auto` interleaves bounded original/specialize/memo/incremental timing samples, uses ProfitEstimate with measured timer/tracking and amortized admission costs, and revisits the choice periodically. It rejects unprofitable actions on the current arithmetic fixture. Explicit modes remain diagnostic. Estimates compare instrumented DBI paths, not whole-game FPS or no-DLL performance.
- Launcher code-cache caps: shared BB 64 MiB, shared trace 32 MiB, private BB/trace 1 MiB each. Private limits apply per thread; DR metadata and application memory are additional, not measured as zero. CPU candidate/cache storage is fixed-size and per-thread cache state retires at thread exit.

Final native process wall totals (one short run each, includes startup and diagnostics; **not frametime or a benchmark confidence interval**): original 37 ms; neutral 184 ms; study 186 ms; auto 189 ms; specialization 243 ms; memo 236 ms; incremental 217 ms. This is an overhead-negative fixture result, not a speedup. All nine execution/control cases passed the state oracle. Actual game compatibility, coverage, usefulness and steady-state own overhead remain unverified.

## Next actionable item

M1–M4 corrections are ready for re-review with targeted evidence; acceptance is still open. Before claiming game acceleration, measure a cheaper execution boundary and genuinely expensive supported regions against a no-ARC baseline. M5–M10 were not requested in this pass and remain pending. Do not repeat unchanged GPU suites or infer Bodycam acceleration from these CPU fixtures.
