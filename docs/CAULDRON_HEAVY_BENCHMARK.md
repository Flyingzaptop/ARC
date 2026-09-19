# Heavy independent DX12 benchmark and single-recording VRS

The small dynamic city remains a diagnostic fixture. The primary GPU workload
is now the official AMD FidelityFX SDK 1.1.4 Cauldron Brixelizer GI sample with
Toyshop/Teddy content. Brixelizer supplies dynamic diffuse/specular GI; this
benchmark does **not** enable FSR upscaling, DLSS or frame generation.
[AMD Brixelizer documentation](https://gpuopen.com/fidelityfx-brixelizer/).

The content contains 3,659,339 instanced triangles, 737 mesh instances, 296 mesh
definitions, 166 material definitions and 443 texture entries across two glTF
files. Entries are asset counts, not a claim that every triangle is visible or
every texture entry is a distinct resident GPU image. The renderer uses texture
maps, raster shadows, animated objects, multibounce GI, specular GI, deferred
lighting, TAA and tone mapping. Native render and display resolution: 1920x1080.

UE5 City Sample was considered: its official recommendation is 64 GB RAM and
at least 8 GB VRAM, above this laptop's roughly 40 GB RAM / 6 GB VRAM. UE is not
installed. A newer FidelityFX 2.3 denoiser sample was also tested during selection;
its ML denoiser did not support this GPU and its default FSR mode was unsuitable.
Neither candidate is used for the final comparison.
[UE5 City Sample requirements](https://dev.epicgames.com/documentation/unreal-engine/city-sample-project-unreal-engine-demonstration).

## Reproduction

Use a fresh directory outside the ARC checkout. Dependencies are an x64 Visual
Studio C++ build toolchain with the Windows SDK and Python. The setup script
downloads the official 1.1.4 release, checks a pinned archive hash, downloads the
official hash-selected media bundle and builds only the required sample/backend.
The archive hash is locally pinned; GitHub did not publish an independent digest
for this release. Assets and vendor sources are not committed into ARC.

```powershell
./scripts/setup-cauldron-benchmark.ps1 -Destination C:/Benchmarks/ARC-Cauldron -Python C:/Python314/python.exe
python scripts/run-cauldron-benchmark.py C:/Benchmarks/ARC-Cauldron/sdk C:/Results/baseline-a
python scripts/run-cauldron-benchmark.py C:/Benchmarks/ARC-Cauldron/sdk C:/Results/current --dll C:/ARC/build/Release/arc-dx12-probe.dll
```

To adapt an existing **dedicated SDK extraction**, run
`prepare-cauldron-benchmark.py SDK_PATH` and build `FFX_BRIXELIZER_GI` in
`ReleaseDX12`. `.arc-original` backups make the patch repeatable. The preparation
script, native build commands and downloads were exercised locally. The combined
setup wrapper was syntax-checked; a redundant second multi-GB installation was
not performed.

The host adapter fixes simulation time to 1/60 s, moves the camera through a
600-pose closed route and retains the asset's animated objects. It waits for
content/modules, warms up for 120 frames, measures 600 frames without removing
slow frames, then saves one PNG after measurement. Additional shorter runs save
three other matched poses. Each process has a 60-second runner budget; final
performance runs took 14–16 seconds including startup/shutdown. Vsync and both
CPU/GPU FPS limiters are disabled. The upstream `-benchmark` option force-enables
a GPU workload limiter, so the adapter explicitly bypasses that module and the
analyzer rejects any limiter timing marker.

The host does not link ARC core or provide effect labels, action parameters,
resources or camera state to ARC. Optional DLL initialization uses the same
generic probe exports. Module labels and native timestamp results go only to
the independent report. Pass timings come from Cauldron's buffered GPU profiler;
they lag the currently submitted pose. Compare aggregate identical trajectories,
not an individual row's camera index against that row's delayed GPU timing.

`frames.jsonl` contains unfiltered cadence, CPU module timings, GPU module timings,
submission, Present and waits. CPU profiling reads thread times for five seconds.
NVIDIA telemetry is sampled once per second, consistently for all four main runs;
those hardware samples include the graphics workload and other system GPU users.
The analyzer uses actual native render resolution from the vendor report. It
does not use the vendor's filtered min/max averages for frame-tail reporting.

## Runtime change

`generic_command_mirror` retains its historical filename/JSON key for compatibility,
but no longer clones or records a second native application command list. It uses
Tier 2 VRS images, updated at submission boundaries. Each in-flight recording
generation owns its map; Reset with a different allocator cannot change an older
GPU generation. Cached-list replay with the policy off uses a neutral 1x1 map.

A bounded pool retains at most 128 control images. Each image has four paired
enable/neutral helpers. A neutral helper is reserved before enabling coarse
shading. If pairs are unavailable, the new attempt stays at 1x1; the render thread
does not wait for helper reuse. Resource reuse is fence-protected. Independent
native recordings do not share a map or acquire cross-queue dependencies.

Bindings persist across compatible draws and are restored before application VRS
changes, unsupported operations, an incompatible pipeline and Close. Existing
application rate images/nondefault rates are preserved. Driver reentry during
native draw/indirect calls is excluded from interception. Lean mode now removes
the unnecessary detailed-observer trampolines; required policy/fallback hooks
remain. Passive mode retains submission interception to neutralize cached maps.

This experimental backend requires VRS Tier 2 and covers the top-left 4096x4096
render pixels. Larger targets get native 1x1 shading outside that extent, as
specified by DirectX. Tier 1-only devices abstain instead of using the old costly
clone fallback. PSO/unsupported API guards, the 60-second lease and independent
quality admission remain necessary. This is not automatic perceptual acceptance.
[DirectX VRS specification](https://microsoft.github.io/DirectX-Specs/d3d/VariableRateShading.html).

## Results: 2026-09-19

RTX 3060 Laptop / i7-11800H. The same host binary, resolution, content, poses and
simulation steps were used in all four runs. Archived manifests pin both host
and DLL hashes. Older v0.1 uses the preserved DLL from the prior dynamic-city run.

| Mode | FPS | 1% low FPS | CPU render-module preparation, ms | GPU span, ms |
|---|---:|---:|---:|---:|
| Baseline A, no DLL | 71.76 | 53.78 | 4.565 | 13.915 |
| Old v0.1 | 61.81 | 45.30 | 14.769 | 13.513 |
| Current single-recording VRS | 72.09 | 47.16 | 6.519 | 13.816 |
| Baseline B, no DLL | 67.68 | 50.39 | 5.475 | 14.736 |

Combined baseline is 69.66 FPS. Current raw difference is +3.49%, while baseline
repeat drift is 6.02%; no reliable net FPS improvement is established. Current
1% low also remains below both baselines. CPU render-module preparation fell
55.86% relative to old v0.1. Sampled process CPU demand fell from about 1.02 to
0.51 equivalent logical cores; baseline demand was about 0.44–0.50 cores.
This is removal of ARC's overhead, not a doubling of the renderer's performance.

The current run recorded 3,630,240 controlled draw calls without native command
copies, completed 720 modified submissions including warmup, and reported zero
Present errors/control faults and zero cross-queue GPU waits. The bounded pool
reached 128 maps; capacity declines during startup are retained in telemetry.
Do not infer full universal command coverage from these counters.

The dominant GPU costs are GI/lighting (about 8.05 ms) and shadows (3.80 ms),
approximately 86% of the current GPU span. G-buffer is 1.28 ms. The GI parent
includes its nested SDF update, tracing/filtering and deferred-lighting passes;
do not add those nested timings to the parent again. Baseline hardware samples
showed 100% GPU utilization. CPU time spent waiting for the next buffer overlaps
GPU work; it is not another render. Driving unused CPU cores to 100% would add no
benefit to this critical path by itself.

All four matched image samples fail the existing coarse-VRS limits. Worst mean
linear error is 0.004018 (limit 0.002), peak about 0.99139 (limit 0.04), worst
8x8 tile 0.20162 (limit 0.008). Baseline repeat itself has temporal GI variation:
mean 0.000282, peak 0.09802; the strict reference peak gate also fails. This is
reported separately, not subtracted to hide damage. The default controller still
abstains; experimental VRS is explicitly disabled at test shutdown.

Validation: 47 non-GPU CTests; native resource/readback/lifetime test; native
invalid-call preservation; VRS tests with the D3D12 debug layer covering direct
and indirect draws, render passes, cached/passive/cross-queue rollback, unknown
operations after a draw, application VRS changes and Reset while a previous GPU
generation is blocked behind a CPU-signaled fence. All passed.

## Next work

The useful targets are GI/deferred-lighting compute work and shadow geometry,
not forcing more CPU utilization or increasing coarse VRS everywhere. First add
generic pass-cost attribution, then safe replacement actions with verified input,
output and synchronization contracts for those expensive passes. Merely lowering
Dispatch dimensions leaves missing/stale output and is not a valid optimization.
Temporal reference construction must also handle the renderer's GI variation
before automatic quality admission can be claimed. The optimizer must discover
these capabilities through DX12 data; the benchmark's private module labels must
not become required engine-specific inputs.
