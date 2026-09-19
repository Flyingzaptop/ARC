# Generic game integration — 2026-09-19

The requested outcome is at least 2x FPS in actual games through automatic,
engine-independent optimization of textures, lighting, geometry and other render
work. **That outcome is not achieved.** No quality mutation was applied in these
game runs; no game FPS improvement is claimed.

## Implemented and checked

- Descriptor metadata now uses bounded slot arrays and reference-counted interned
  values. Bodycam previously saturated a 65,536-entry map. The new run observed
  546,120 non-null descriptor slots using 3,791 distinct metadata values, without
  that saturation. Metadata equivalence does not mean descriptors are interchangeable
  for rendering: this representation is observer-only.
- Root binding, raster and pipeline state are recorded only during requested graph
  captures. Unsupported calls remain counted, but do not take the observer mutex
  and invalidate a graph when no graph capture is active. A native microbenchmark
  of 100,000 viewport/scissor pairs changed from 6.784 to 2.6034 ms. This is observer
  overhead evidence, not game speedup evidence.
- Initial open command lists created during capture are now observed without
  requiring a Reset. The regression test first failed with a missing copy
  dependency, then passed after the CreateCommandList hook was added.
- `arc-dx12-probe-launch --image PID DLL NEW_JSON` requests one asynchronous native
  backbuffer readback. It uses the swapchain's associated direct queue, restores
  PRESENT state, fences its own copy, and maps/writes only in the worker after GPU
  completion. No render-thread GPU wait is introduced. Readback still has CPU/GPU
  cost and should be sampled sparsely.
- One bounded image job is allowed; existing output is refused. Native RGBA8,
  BGRA8 and packed RGB10A2 bytes are preserved without assuming a transfer function.
  Unsupported/protected swapchains are refused. A signal failure retains the
  bounded GPU job rather than destroying potentially in-flight resources.
- Native tests verify every pixel of a known 61x37 clear, including row padding,
  ordinary GPU copy data, command replay, resource retirement, and original HRESULT
  preservation. All 46 non-GPU CTest tests passed.

## Actual Bodycam evidence

Local source evidence: `C:\Users\r3d_flzp\ARC-Hardening-GPU\bodycam-readback-20260919-144734`.
The launch manifest records the exact tested DLL SHA256. This game binary predates
the final initial-command-list hook and protected-swapchain rejection; those final
changes have native validation, not another game run.

| Measurement | Capture 1 | Capture 2 |
|---|---:|---:|
| Resolution | 1280x720 | 1280x720 |
| DXGI format | R10G10B10A2_UNORM | R10G10B10A2_UNORM |
| Readback copy GPU time | 0.413952 ms | 0.611648 ms |
| Enqueue CPU time | 1.4309 ms | 2.1716 ms |

Both images completed and were decoded/visually inspected. The recorded process
snapshot had 2,290 Presents and zero Present failures; those totals include scene
startup and are not a timed FPS result. GPU copy duration is not GPU frame duration.

The intended input window was 60 seconds; the last UI observation was at 66.086 s,
then the game was closed. Inputs were short W/A/S taps and a mouse drag. This is
**not a sustained traversal or dynamic performance benchmark**. The current UI API
does not expose held-key duration or raw relative mouse motion. Do not relabel this
limitation as a passed dynamic test.

## Still required for the user goal

1. Reliable 60-second moving-scene measurements with comparable baseline/modified
   conditions, frame-time distributions and the same resolution/settings.
2. Complete enough resource/work coverage: root ranges and root GPU addresses,
   indirect work, allocation variants and descriptor copies still have gaps. An
   unknown resource binding must not authorize mutation.
3. Comparable image references under motion and known color interpretation. Current
   images explicitly report `reproducible_state:false`, `color_space_known:false`,
   and `gpu_frame_timing_available:false`.
4. Reversible generic actuation tied to the existing independent critic and rollback
   controller, with validated savings across render domains. Sampler/texture changes
   alone do not satisfy the requested lighting/geometry scope.
5. Verified game-level >=2x FPS gain at acceptable image quality. Synthetic results
   and instrumentation microbenchmarks cannot satisfy this gate.
