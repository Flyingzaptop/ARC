# Full ARC comparison (Wicked + Cauldron)

This integration keeps the source-assisted Wicked draw consumer from `63b0379`.
It does not implement automatic CPU extraction or an engine-independent draw adapter.

## Session and ownership

`host_cpu_offload: true` in the ordinary automatic-session JSON permits a cooperative
host adapter to run. `ArcHostOffloadEnabled` returns 1 only while that session is
running, not cancelled, and explicitly permits host offload. Missing exports,
missing session, configuration false, and stop use the original CPU branch.

`src/adapters/wicked/resident_draw_queue.hpp` owns Wicked GPU buffers and the existing
applicability certificate. The device owner calls shutdown after its existing final
GPU wait. Disabling stops new substitutions; it does not destroy in-flight resources.
No new CPU waits, result readback, or frames in flight are added in performance mode.
Oracle mode remains a separate, expensive correctness-only mode.

`ArcHostShaderScope` excludes adapter-owned shader creation from candidate discovery.
`ArcHostWorkScope` excludes its sorting dispatches from *game-pass cost selection*.
It does not suppress state/binding interception. Whole-frame GPU timestamps still
include this work; a separate ResidentQueue GPU range records its cost.

The legacy Wicked host-quality adapter remains off deliberately: full mode here is
**generic ARC DLL/controller + session-bound resident draw adapter**, not the older
engine-aware quality presets. The manifest records these separately.

## Reproduce using the saved package

Prerequisites: the existing Wicked assets/SDK directories, Python 3.14, NVIDIA
telemetry utility, and pinned DXC installation on this machine. This is a ready
benchmark build, not a standalone redistributable game/asset package.

```powershell
./scripts/compare-full-arc.ps1 -Output C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-repeat
```

The wrapper validates package hashes, refuses to interfere with running benchmark
renderers, installs the two saved hosts/shaders temporarily, and restores previous
files afterward. It runs A-B-B-A on both hosts and generates the six SVG plots.
Commercial games are never launched.

A: same instrumented renderer, original execution, no injected ARC DLL.
B: DLL plus automatic target-feedback controller, fixed 120 FPS, Aggressive profile;
Wicked additionally permits resident CPU offload. Cauldron CPU offload is unsupported.
No upscaling, frame generation, global resolution reduction, or power cap is enabled.

Wicked uses its animated Instances scene with a fixed camera. Object animation is
phase-aligned to measurement start and continues during the 20-second window.
Warmup is 20 seconds from instrumentation initialization. The optional camera-motion
fixture failed the nonempty-image oracle and is not used for these comparisons.
Cauldron uses the existing primary periodic route; measurement starts on its first
600-frame boundary after 100 seconds, with a 120-second warmup ceiling, then runs
20 seconds. The existing runner bounds the entire process to 170 seconds.

## Measurement contract

CPU processing-through-submission includes the host work and waits inside that
interval. Successful Present return intervals include intervening CPU gaps.
`1000 / mean(Present interval)` is Present-call cadence in Hz, **not displayed FPS**.
No compositor/display tracing is claimed.

Wicked GPU query slots retain the source device frame ID until retirement. Cauldron
query slots likewise retain their source scene frame; its GPU timings getter now
returns the just-collected slot, consistently with the span and source ID. GPU values
are joined by that ID. Unretired tail frames remain missing. Overlapping queues are
not added together: the reported graphics span includes ordered offload consumers.

All samples/outliers remain in CSV and plots; lines are not smoothed or clipped.
Policy/controller decisions and failed profiles are retained in the raw archive.
The source patches were applied to captured pre-integration sources and compared
against the compiled source files, including normalized line endings.

## Source reconstruction

`wicked-integration.patch` applies on the locally instrumented `63b0379` Wicked
host (after `experiments/wicked-resident-consumer/resident.patch` and its documented
prerequisites). It includes the session binding, shutdown/stop test hooks, measurement
and deterministic object-animation changes. The tracked adapter header is identical
to the resulting `WickedEngine/ArcResidentQueue.h`.

`cauldron-integration.patch` applies under `sdk/framework/cauldron/framework` after
the existing ARC Cauldron benchmark preparation. It changes only measurement;
no scene/render algorithm or shader is modified there.

Build commands and measured binary hashes are retained with the report/evidence.
The historical experiment files are kept unchanged for independent reproduction.
