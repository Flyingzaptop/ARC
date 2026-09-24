# Wicked resident draw preparation — source-assisted experiment

Base ARC: c7711e9. This is not automatic binary discovery or a universal ARC backend.
See docs/reports/2026-09-24-resident-draw-consumer.md for results and limitations.

## Applying to the recorded harness

The target is the already instrumented Wicked checkout used by c7711e9, with its
corrected synchronous/async patches and post-join list checker. This patch is not
for an arbitrary pristine upstream checkout. `manifest.json` fingerprints the
before/after files. Run `git apply --check resident.patch` in the Wicked root first.

Copy ArcResidentQueue.h and ArcResidentProbe.h into WickedEngine; copy
ArcResidentDebug.h into Samples/Tests. Apply resident.patch. Copy
arcResidentQueueCS.hlsl and arcResidentPixelsCS.hlsl into WickedEngine/shaders.

Compile both shaders with the pinned DXC 1.9.2602.24, target cs_6_0, entry main,
-Gis and -I pointing to WickedEngine/shaders. Write the corresponding .cso into
Samples/Tests/shaders/hlsl6. The loader reads these exact binaries, avoiding silent
recompilation with different flags. Final .cso files are also in the raw ZIP.

Build Samples/Tests/Tests.vcxproj with VS18 MSBuild, Release/x64, explicit SolutionDir
(with trailing slash), ArcRepoRoot=ARC-perceptual, and
ArcBuildRoot=ARC-perceptual/build-wicked-generic (the MT CRT build).
Set temporary environment `_LINK_=/LTCG /INCREMENTAL:NO`: incremental LTCG hit C1001;
full optimized LTCG succeeded. Do not disable Release optimization.

Existing local command:

```powershell
$env:_LINK_='/LTCG /INCREMENTAL:NO'
& 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/MSBuild/Current/Bin/MSBuild.exe' `
  "$WickedRoot/Samples/Tests/Tests.vcxproj" /p:Configuration=Release /p:Platform=x64 `
  "/p:SolutionDir=$WickedRoot/" "/p:ArcRepoRoot=$ArcRoot" `
  "/p:ArcBuildRoot=$ArcRoot/build-wicked-generic" /m:2 /v:minimal /nologo
```

## Modes

- ARC_RESIDENT_QUEUE=0/unset: original queue preparation; default off.
- ARC_RESIDENT_QUEUE=1: GPU-resident draw preparation for the admitted class.
- ARC_RESIDENT_QUEUE=2: same GPU consumer plus independent CPU reference queues,
  canonical per-key membership/pointer checks, exact float32 distance bits, exact
  half keys, and same-frame original CPU reference prepass. Primitive-ID and depth
  must match at every pixel; nonzero coverage is required. Not a performance mode.
- ARC_RESIDENT_SCREEN=1: bounded CPU pass timing/scene-shape diagnostics only.
- ARC_RESIDENT_DEBUG=1 with launch argument debugdevice: D3D12 diagnostic messages.

Keep ARC_CONTROLLED_CULL=0, ARC_ASYNC_CHAIN=0 and ARC_VERIFY_FINAL_LIST=0 for these
measurements. The previous implementation and its fixes remain intact but disabled.

## Correctness and ownership

CPU retains scene/ECS, world matrices, bounds, visibility, resource management and
original PSO/draw selection. Checks are piggybacked on existing object updates.
Current visible IDs and actual sort_bits are copied once (8 bytes/item).
GPU generates exact keys, sorts, packs raw pointer words and feeds existing shaders.
There is no algorithm-result readback or new CPU fence wait in mode 1.

CPU metadata/resources are prepared before dispatching render jobs. The Slot pointer
is captured by value. GPU work follows instance uploads on cmd_prepareframe. Existing
command dependencies and device frame retirement protect a bounded two-slot pool.
The original buffer count/frames in flight are unchanged. Secondary views and
unsupported objects/passes use the original path.

Admission restricts mesh/LOD/stencil and opaque flags, fading/dithering, numeric range,
camera stability, CPU rounding mode, and GPU int64 support. It is NOT an equivalence
proof for arbitrary scenes. Equal sort keys may have different order; exact per-key
membership AND the independent pixel/depth oracle are required for each new scene.
Numerical tolerances were not widened. FP32 sqrt rounding is corrected by integer
midpoint comparison; cpu_half follows the pinned CPU conversion instead of legacy
f32tof16. test_exact_sqrt.py checks the correction against an independent host oracle.

The main depth surface lacks SRV support. Diagnostic mode uses a separate readable
copy; this copy and all image/readback checks are absent in mode 1.

## Re-running

Preserve the original saved EXE and copy the new binary to
<raw>/Tests-resident-final.exe. run-matrix.ps1 performs A-B-C / C-B-A, records NVML,
uses the unchanged profile-wicked-cpu.ps1 15-second cap, and restores the original
EXE in finally. It refuses existing CPU CSV output through the shared harness.

`python analyze.py <unpacked-raw-directory>` reproduces the comparison/plot using
only the Python standard library. Raw inputs, launch parameters, hashes, shader
binaries and failed diagnostic attempts are committed in the evidence ZIP.

The measured metric is CPU submission cadence. GPU range durations are retired
samples, not same-row CPU-frame attribution and not display FPS. Current NVML data
shows ~62-65 W under active work, not a verified 30 W limit. No power policy was
changed by this experiment. New geometry/hardware needs fresh validation.
