# Bodycam: negative live ARC test, 2026-09-22

Build: 37a16c4 (binary SHA-256 manifest stored with the session). User manually drove the route. Baseline had no ARC DLL; optimized run loaded the current frozen DLL before the game entry thread resumed, including child-process handoff. Direct launch initially triggered Steam restart without ARC; that attempt was excluded. Supplying the installed game's SteamAppId/SteamGameId allowed the observed injected launch. No persistent Steam launch options were added.

PresentMon 2.6.0 from Intel's official GitHub release, valid Intel signature. Unelevated attempt failed ETW access; the hotkey attempt produced no CSV. Both are excluded, preserved and not presented as measurements. Actual measurements used elevated timed capture with 5s delay and 60s recording. The user confirmed UAC. One baseline and one ARC window, one swapchain each, manually repeated route, no camera-pose equivalence proof or image-quality claim. Thermal/power variation was not measured in this comparison.

ARC: target 95 FPS, target_feedback, aggressive, overlay disabled. No game graphics-setting or final-resolution change was requested.

| Metric | No ARC | ARC |
|---|---:|---:|
| Captured frames | 2850.000 | 1356.000 |
| Span, seconds | 59.902 | 59.853 |
| Mean FPS | 47.556 | 22.642 |
| 1% low FPS | 23.041 | 13.193 |
| Mean frame, ms | 21.028 | 44.166 |
| p95 frame, ms | 27.474 | 57.965 |
| p99 frame, ms | 35.691 | 64.460 |
| Max frame, ms | 172.737 | 191.840 |
| GPU Busy, ms | 18.247 | 19.223 |
| GPU elapsed, ms | 20.378 | 40.667 |
| GPU Wait, ms | 2.130 | 21.444 |
| Frames >50ms | 1.000 | 270.000 |

Observed mean FPS fell 52.39%. GPU Busy did not fall (18.25 -> 19.22 ms), while GPU Wait grew 2.13 -> 21.44 ms. This indicates a large increase in gaps/stalls, not successful reduction of GPU work. PresentMon CPUBusy is a frame-timing estimate, not CPU execution time summed over all game threads. Without component ablation/CPU trace the slowdown cannot be assigned precisely to a particular ARC lock, profiler or compiler.

## Actual functionality

- Early DLL connection and logging: confirmed. 276 root signatures, 4440 compute PSOs observed.
- Modified compute dispatches: 0; prepared compute variants: 0.
- Modified pixel draws: 0; prepared pixel variants: 0.
- VRS modified draws/submissions: 0.
- Therefore mip, density, PCF, sample reduction and inline RT were not demonstrated in this game. The controller ran, but did not execute any useful optimization.
- Shadow cache remains a standalone mechanism, not a live Bodycam feature.

## Observed blockers

791 shader declines: 774 shader_model, 10 unproven_local_memory, 3 group_barrier_convergence_unproven, and one each atomicBinOp.i32, rawBufferStore.i32, textureStore.i32 and thread_dimensions. Another 3649 PSOs awaited measured cost. The source analyzer currently explicitly admits cs 6.0..6.5 only; this is a coverage limit, not proof that removing the check would make newer shaders safe.

Profiler: 5 faults, repeated unsupported segments and capacity declines. Controller reported unknown bottleneck / retained policy, with neutral pixel controls and VRS off. Separate pipeline_or_worker_capacity counter reached 1015. Pixel preparation did not start, so its zero count alone does not prove all pixel shaders unsupported.

## Stop state

ArcStopOptimizer succeeded; session stopped with restoration_confirmed=true. Explicit profiler stop was requested. ArcUsePassiveMode still returned status 2 (profile busy); five recording references remained in the diagnostic snapshot. Therefore full interception shutdown is NOT confirmed. Restarting Bodycam without ARC is required to remove the DLL entirely; no forced unload was attempted.

## Next concrete work

1. Fix profiler termination and bound unsupported profiling/analysis costs; no supported actuator should mean a cheap dormant state.
2. Inspect the actual rejected shader models and implement their resource/handle semantics with native tests, rather than widening the version predicate alone.
3. Fix coverage of the game's command/PSO paths and obtain valid measured cost evidence.
4. Prove one actuator actually executes in Bodycam, then repeat baseline/neutral/active with the same route before any gain claim.

Raw CSV, manifests, copied decisions/profiles, before/after snapshots and comparison.json:
C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/bodycam-check-20260922
