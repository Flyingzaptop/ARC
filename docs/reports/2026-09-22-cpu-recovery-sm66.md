# CPU overhead recovery, bounded discovery and SM6.6

## Delivered

1. Empty GPU-control submission path returns before descriptor/state mutexes when no variant pool has ever been published. A monotonic acquire/release flag preserves the original path after the first publication, including cached recordings and rollback. The interceptor still submits original commands exactly once. Owned fixture: 100 submissions, prepared=0, empty_control_bypasses=100, no hook failures, healthy device. This is a proven bypass, not a measured Bodycam FPS gain.
2. Profiler stop no longer remains busy forever on an application-owned open command list. After the five-second drain deadline, measurement is abandoned while its query storage remains retained for native recording/submission lifetime. Such evidence is labelled abandoned and not used for bottleneck inference. Regression reproduced failure before the fix; after it, passive mode succeeds and later closing/submitting the application's original list passes D3D12 validation.
3. Span-capacity failure stops the current capture. Incomplete/unsupported profiles do not become valid CPU/GPU bottleneck evidence. Failed discovery retries back off 4/8/16/30 seconds; context churn cannot bypass the cooldown. With at least three unusable profiles, at least 30 elapsed seconds and no active policy, controller stops rather than continuing expensive unproductive work. Deferred passive transition waits for restoration and capture completion; activation generations prevent an obsolete request disabling a new session.
4. Owned integration fixture deliberately alternates 260 dispatches per command list. It completed 1800 Presents, stopped for unsupported_profile_runtime_stopped, confirmed restoration, entered passive hooks and retained a healthy GPU. Test source mode --profile-overflow is diagnostic only.
5. Compute cs_6_6 static createHandleFromBinding/annotateHandle support added. Range, space, resource class, index and required annotation are checked; generated control CBV and execution-marker UAV use modern handles. Dynamic indices, unproved heap/bindless access, unknown models and side effects still decline. Added DXC/validator negative tests and GPU oracle: original/neutral, 2x2, runtime 1/2 and original rollback. Full shader worker controlled-proof with CBV/SRV/sampler/UAV/SampleLevel validated. Existing pixel, PCF, sample/ray and GPU-control regressions passed.

SM6.6 edge/spatial/sample/ray auxiliary transformations that require legacy handle patterns remain unavailable where their matchers cannot establish applicability. No claim that all Bodycam shaders or all 774 prior shader_model declines are fixed: the actual rejected Bodycam IR was not captured in that test and no new game run has occurred.

## PRIMARY: application CPU object work

NOT solved. No engine-specific LOD/culling adapter was added. Skipping a draw after the game constructs it cannot remove preceding CPU scene traversal, object update, culling or LOD selection. Affinity/priority changes are not a substitute for fewer computations. The clean baseline had about 18.25 ms GPU Busy for a 21.03 ms mean frame; those figures and PresentMon CPUBusy alone do not establish that the unmodified game is exclusively CPU-bound.

Prepared a graphics-API-independent next diagnostic: scripts/arc-cpu.wprp and scripts/capture-cpu-work.ps1. CPU samples, CSwitch, ReadyThread, image/process lifetime and hard-fault events; 32 MiB configured trace buffers, 5..60 second capture, dedicated named WPR instance, 512 MiB temporary-file soft stop, target PID/lifetime manifest. System-level events require target filtering during analysis. The script neither injects ARC nor changes affinity/game settings. WPR accepted the profile schema and event/stack configuration; PowerShell parser passed. Actual elevated capture, lost-event review and analysis remain unperformed. Without game symbols some stacks will remain module+offset.

Once a true pre-DX hot operation is identified, investigate a narrowly proven engine-independent transformation (e.g. exact-input caching of a repeated pure operation); do not replace that evidence with arbitrary object removal or a speculative compiler cache unrelated to the observed hot path.

## Evidence

- .../ARC-Hardening-GPU/universal-optimizer/cpu-recovery-profiler-before: regression fails.
- .../ARC-Hardening-GPU/universal-optimizer/cpu-recovery-profiler-after and cpu-recovery-profiler-integrated: passes, open recording safe after stop.
- .../ARC-Hardening-GPU/universal-optimizer/cpu-unavailable-runtime-01: 1800-frame circuit-breaker integration fixture.
- Repo traces/universal-optimizer/cpu-empty-bypass-20260922-sol: 100 empty-control bypasses.
- .../ARC-Hardening-GPU/universal-optimizer/cpu-sm66-regression-final: legacy native regression.

No new commercial-game performance result. CPU overhead is reduced in specific paths and bounded on unsupported workloads; not claimed eliminated. Report remains partial on the user's primary application-work objective.
