# Full-frame 30 -> 60 experiment

Game testing is reserved for the user. This experiment uses only an ARC-owned
D3D12 renderer. No game process or Steam launch is part of this work.

Predeclared question: can a safe mip-view action remove approximately half of a
complete 1080p frame's critical-path time, rather than accelerate one micro-operation?

The workload must generate and compose all output pixels, copy the final image to
a swapchain and call Present. Report both full GPU frame timestamps and CPU-to-GPU
completion/Present wall time. Reciprocal GPU time is a ceiling, not measured game FPS.
Reference readback/critic cost is measured separately from steady-state rendering.

Use a baseline-only workload calibration near 33.3 ms, then lock the workload for
all arms. Do not multiply the old microbenchmark's gain, insert sleeps, repeat
discarded work or adjust workload after seeing optimized results.

Scenarios: bandwidth-dominated smooth material (potential positive), unavoidable
lighting-dominated work (Amdahl control), high-detail material (image-damage control).
Each pass must contribute to the final image. Calibration parameters and all pass
times are retained; a deliberately bandwidth-heavy case is not evidence about the
distribution of real-game workloads.

An isolated offscreen probe may investigate high-importance work; importance still
orders candidates, and the same independent image/timing guard decides retention.
This capability does not authorize probes against a user's live presented game.

Acceptance for the positive case: baseline full-frame wall p50 28-38 ms, modified
wall p50 <= 16.667 ms and >=2x speedup, independent final-image guard PASS, full
restoration, and stable before/after baselines. Repeat with counterbalanced arm
order and retain raw timings/images and negative outcomes. Controls must not be
reported as 2x success or retain a visually damaging change. If these gates fail,
report FAIL and the measured limit rather than relaxing the gates.

## Exploratory corrections retained

The first microbenchmark-derived full-frame workload had an almost uniform output
and sampled the texture globally. It reached roughly 30 -> 169 throughput FPS,
but is an intentionally inefficient bandwidth capacity demonstration, not realistic
renderer evidence. Its first probe failed timing drift; equal one-second warmup
then removed the observed mismatch. Both outputs remain in the archive.

The spatial revision preserves UV gradients and sums spatially varying light
contributions instead of a recurrence that converged to a uniform color. Material
samples use a local footprint, improving baseline locality. Initial calibration
hit its 1024-sample cap around 23 ms rather than 33 ms. The final baseline-only
calibration cap is 2048 samples (same rule in all material scenarios); arm parameters
are frozen thereafter. This change targets test duration, not a chosen speedup.
The final verdict uses this spatial workload, not the earlier easier positive case.

## Multi-domain extension requested by the user

The texture-only spatial experiment measured 31.4 -> 61.6 serialized throughput
FPS (1.96x), retained as formal FAIL against the predeclared >=2x gate. The next
experiment changes the available actions, not the quality/benefit thresholds:

| Action mask | Action |
| --- | --- |
| 2, 3 | Finest accessible texture mip 2 or 3 |
| 0x10, 0x20 | Lighting integration samples divided by 2 or 4 |
| 0x100, 0x200 | Shadow visibility rays divided by 2 or 4 |
| 0x1000, 0x2000 | Shadow buffer dimensions divided by 2 or 4 |

Four passes generate lighting, materials, visibility shadows and composition;
the final image is copied to the swapchain. Shadow rays test intersections with
eight spheres and sample an area light in a compute shader. This is actual ray
intersection work, not hardware DXR or a commercial-engine implementation.
All light samples represent integration of the same field: changing their count
does not delete scene lights. Coarse shadow images are sampled in composition.

Test workloads emphasize material cost, lighting cost, fine material detail, or
shadow-ray cost. Every candidate goes through full/modified/full image and timing
verification. Only independently accepted per-domain actions are proposed as a
combination, and the combination must pass its own fresh critic evaluation.
Selection uses measured gain; the core contains no renderer labels or scene IDs.
The CPU experiment driver chooses candidate masks and supplies isolated captures;
this is not a new generic game interception capability.

Each workload is calibrated ONCE using full quality. It then stays fixed through
all candidates and three ABBA/BAAB/ABBA verification rounds. Failed candidates,
raw final images, per-pass ticks and workload parameters are retained. The
lighting-dominated case becomes an optimizable scenario once lighting actions
are available; it remains an Amdahl control for texture-only actions.

Scope: procedural multipass compute renderer and swapchain, not a polygonal game
scene. Explicit LOD-zero baseline models excessive texture detail; a renderer
already choosing an appropriate mip may have no such reserve. Hundreds of texture
samples/pixel are a stress case. Screenshots are inspected, and raw images retained.
