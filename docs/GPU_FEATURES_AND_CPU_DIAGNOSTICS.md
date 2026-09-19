# GPU features and CPU diagnostics

The optimizer must not depend on continuously copying screenshots to the CPU.
`arc-dx12-probe-launch --features PID DLL NEW_JSON` now runs a compute reduction
against the native DX12 backbuffer on its associated direct queue. Each 16x16
tile produces four floats: encoded luminance mean, variance, maximum local edge,
and valid pixel count. Only these statistics cross to CPU; no full image is saved.
The original PRESENT state is restored. A completed GPU fence precedes CPU access.

Shader and driver pipeline compilation happen on the request thread outside the
runtime mutex. At most four device pipelines and one completed scratch allocation
are cached. Scratch storage is reused only after GPU completion and CPU consumption;
the application queue and source backbuffer are not retained by the spare job.
The request remains explicit and bounded, not enabled automatically every frame.

Native tests check exact clear-color statistics, partial edge tiles, a changed
half-screen image, nonzero variance/edges, and reuse on the next backbuffer.
The reused 61x37 native test recorded 0.2655 ms enqueue CPU time and 0.008192 ms
GPU analysis time. These tiny-fixture timings are not a game-performance claim.

In FPV.SkyDive at 1920x1080, one actual GPU feature capture transferred 130,560
bytes instead of 8,294,400 image bytes (about 63.5x less). Analysis cost was
0.129344 ms GPU. This game run used the version before scratch-buffer reuse;
its 3.889 ms enqueue CPU cost must not be represented as the reused path's cost.

These features are not a perceptual acceptance verdict. Color interpretation,
motion correspondence and reproducible reference state remain unproven. Full
images are still available as a separate, sparse verification path.

## CPU process data

`arc-process-sample PID SECONDS NEW_JSON` reads per-thread CPU time, cycles and
thread names from an explicitly selected process. Duration is bounded to 1–60 s.
It does not inject, suspend threads, modify priorities or inspect process memory.
It reports sampling limits, including newly created threads not covered by the
initial inventory. In a five-second Bodycam run without ARC injection, all 150
sampled threads were readable and their total CPU time matched the process total.
RenderThread/RHIThread and four Bink video workers were among the large CPU users.
This identifies where to investigate; it does not prove a function-level cause.

PresentMon 2.5.1 was downloaded from its official release and its published SHA256
verified. Its ETW session was denied for insufficient privileges. No account,
security setting or privilege was changed to work around that denial.

## Correct baselines

`--passive PID DLL` retains only Present/Present1 hooks and disables the render-call
hooks. Re-enabling an experimental mode restores those hooks; native tests verify
that both passive execution and subsequent cached-list rollback preserve pixels.
Timing output now identifies foreground/background Presents, so background
throttling cannot silently qualify as a foreground benchmark.

In the same Bodycam shooting-range hub and launch configuration, short foreground
windows measured 30.17 FPS passive, 27.11 with the updated VRS experiment, and
30.50 after restoring passive execution. The no-injection hub screenshot also
showed approximately 29 FPS. The user's reported stock ~45 FPS is a separate
reference; the discrepancy is not resolved by these measurements. Launch options,
scene and settings must match before claiming any gain against that reference.

FPV short windows measured 93.87 / 82.23 / 94.10 FPS (passive / VRS / restored).
The forced-DX12 log also reports `KGenLut3D_NoTonemap` missing before any quality
mutation, including older observer builds. A clean no-injection reproduction is
still needed to establish whether this is an application/backend limitation.
These are rejected experiments, not successful optimization or verified dynamic
traversals. No 2x game FPS result is claimed.

Bodycam's existing Engine.ini contains `r.OneFrameThreadLag=0` and frame-interpolation
overrides. Those settings were read as diagnostic evidence and were not changed.
Engine-specific configuration changes would not establish an engine-independent
optimizer result.
