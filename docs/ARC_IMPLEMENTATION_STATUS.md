# ARC implementation status

Execution ledger for `ARC_AGENT_IMPLEMENTATION_PLAN.md` (2026-09-23). Status labels describe this plan's deliverables, not ARC's overall maturity. Update this file at each milestone; do not infer performance, quality, or game support from implementation alone.

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
| M1 Contracts and bounded observation | implemented / local_correctness / native_checked; game_pending, benefit_unknown | `candidate_contract.hpp`, `discovery_budget.hpp`, `cheap_observer.hpp`; connected compute/pixel publication gates, shared worker quota/cancellation, capture quotas, cache persistence, idle observation/rearm | Focused 4 suites and owned native cases in `docs/evidence/M1_CHECKS.json`; no commercial games | Per-check source/transitive inputs, Release DLL hash, DXC hash and driver inventory in evidence JSON | M1 exits verified within declared runtime coverage. CPU DBI is deliberately unavailable until M2. Next: M2 ranked synchronized discovery. |

| M2 CPU discovery | pending | `scripts/arc-cpu.wprp`, `scripts/capture-cpu-work.ps1`; CPU recovery report | Clean Bodycam ETW capture and thread sample were collected at 3b56c4b; no lost events/buffers. That window has no simultaneous GPU/frame correlation | M1 contract + capture scripts + parser version + target process lifetime | No ranked region inventory, synchronized frame/GPU correlation, or DBI backend. Next: bounded capture/parser and Windows DBI feasibility with pinned dependency. |
| M3 CPU execution / specialization | pending | No connected pre-DX CPU executor evidenced. `src/backends/dx12/` work does not satisfy this phase | None for pre-DX CPU execution | M1 publication contract + M2 discovery/backend and analyzer versions | No automatically discovered exact CPU variant, guard fallback, or owned-process execution evidence. Next: implement the narrow supported region end-to-end. |
| M4 Memoization / incremental recompute | pending | No qualifying pre-DX CPU path evidenced | None | M3 executor + dependency/key model + generations | No exact memoization or bounded incremental recomputation connected to real CPU work. Next: use M3-discovered work and explicit dependency completeness. |
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
- No-candidate runtime keeps Present/submission and PSO-creation hints active. Heavy profiling uses cooldown; novelty or a budgeted retry rearms discovery. Hint loss only reduces diagnostics; an observation gap advances correctness generation and invalidates dependent admission. Creation hints are not code identity/proof and do not retain idle-only PSO bytecode.
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

CPU study quotas are ready for M2; runtime explicitly reports `cpu_capture_backend_available=false`. This is not a pre-DX CPU optimization claim. Existing GPU query/recording caps remain additional independent bounds. Own meter microcheck: about 1.35 ns disabled and 66.91 ns enabled per empty call on this host; no overall game-cost claim follows. The 0.2 ms CPU / 0.25 ms GPU goals and real-game usefulness remain unverified.

## Next actionable item

M2: synchronize ETW/frame evidence, rank stable module-relative regions, pin and prove a bounded Windows DBI discovery path. Reuse M1 contracts, queues and result fingerprints. Do not restart the repository audit or require a new game run before independent M2 code work.
