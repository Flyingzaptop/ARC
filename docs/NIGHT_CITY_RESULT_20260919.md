# Full-frame optimization: dynamic night-city experiment

## Result and exact scope

The owned procedural 1080p night-city flythrough passed the predeclared 2x gate:
**34.23585 -> 8.13785 ms**, or **29.21 -> 122.88 serialized render/Present
throughput FPS (4.207x)**. This measures the complete synthetic GPU frame plus CPU
recording/submission, GPU completion and Present, not one isolated operation.
It does not measure screen scanout, game CPU simulation or a commercial game's FPS.

Source: `d05a6b2` (binary/source SHA-256 provenance is in the run manifest).
Games and Steam were not launched during this work; game testing is reserved for
the user. Core policy remains renderer-independent.

## Scene and comparison

The scene uses perspective camera motion along a street, 28 ray-intersected
buildings, emissive windows/signs, a sampled area-light field, soft visibility
shadows, fog, lane markings and analytic wet-road glow. No Cyberpunk assets are
used. Intersection work runs in a compute shader, not hardware DXR.

Each measured block replays exactly camera ticks 0..60. Three rounds alternate
full/modified/modified/full, then the reverse, then the original order. All quality
trials compare full/modified/full 2x2 mosaics of full-resolution views at ticks
0,20,40,60. Start/end images differ substantially (mean absolute change 0.0922),
so this is not a static image with a moving counter.

The worst modified-vs-reference channel error at the checked waypoints was
0.0039216 (about one 8-bit step). Mean error was 0.000000539 and worst 8x8 tile
mean 0.000143. The unchanged critic thresholds passed. Both full-quality reference
mosaics matched. A final restored frame was rendered.

Four image waypoints do **not** establish absence of flicker or intermediate-frame
artifacts. No temporal reconstruction/denoiser is modeled. The visual preview is
made from actual GPU readbacks; its playback speed is illustrative, not measured FPS.

## Decisions across render domains

Every candidate was independently tested, including those that failed:

| Domain | Candidates | Decision on this flythrough |
| --- | --- | --- |
| Texture detail | Finest SRV mip 2 or 3 | Both passed; mip 3 had larger validated gain |
| Lighting integration | Half / quarter of the samples | Rejected for image damage |
| Soft-shadow rays | Half / quarter of the rays | Rejected for image damage |
| Shadow resolution | Half / quarter in each dimension | Rejected for image damage |

No combined action was proposed for the city because only one domain passed.
The controller did not force every available knob down to get a speedup.

The measured GPU pass intervals explain the result (representative first block):
lighting about 0.66 ms, material 29.64 -> 3.41 ms, shadow visibility about 2.7 ms,
composition about 0.47 ms, swapchain copy about 0.04 ms. Work outside the material
pass did not disappear. Full-frame speedups across the three paired rounds were
at least 4.21x, 4.18x and 4.19x, with stable before/after baselines.

## What this does and does not prove

The test deliberately calibrates full-quality material sampling to a ~33 ms
baseline **before** any optimized measurements, then freezes all workload settings.
The city ended at 301 material samples/pixel with explicit LOD zero, 16 lighting
samples and four shadow rays. This is an intentionally excessive sampling workload
with smooth materials, not a representative modern engine preset. An engine already
using appropriate texture LOD may have little or none of this reserve.

Thus the evidence supports: the implemented action/critic mechanism can exploit
a large known GPU reserve in a complete moving-frame workload. It does **not**
support: ARC can make any 30-FPS game run at 60+ FPS, or this benchmark predicts
typical game gains. The initial question about widespread real-game reserves
remains open.

Candidate exploration took **91.10 seconds** including warmups, repeated probes,
readback, CPU comparison and artifact I/O. This is offline qualification, not a
live adaptive loop suitable for gameplay. Naively amortizing the complete cost
requires about 3,491 improved frames. Making exploration cheap/asynchronous and
proving real-game capture/action capabilities remain required product work.

## Other owned controls, not game forecasts

The multi-domain static matrix (`d4adc90`) also retained these results:

| Workload | Full -> modified throughput FPS | Outcome |
| --- | ---: | --- |
| Material-dominated | 32.03 -> 66.95 | 2.09x, image accepted |
| Lighting-dominated | 29.92 -> 110.49 | 3.69x, quarter lighting sample count accepted |
| Fine material detail | 31.48 -> 49.49 | Rejected: image damage; modified speed is an oracle measurement only |
| Ray-heavy soft shadows | 29.88 -> 876.04 | 29.32x stress-case capacity; quarter rays + quarter shadow dimensions jointly accepted |

The extreme shadow number reflects deliberate oversampling of a simple scene;
it is not promoted as a realistic game expectation. Combined actions underwent a
fresh image trial instead of assuming individually safe changes remain safe together.

Earlier texture-only spatial measurements reached 31.39 -> 61.57 but only 1.96x;
their strict 2x result remains FAIL. The earliest almost-uniform bandwidth test and
its reference timing-drift failure are preserved as exploratory evidence, not the
headline result. Gates were not lowered.

## Reproduction

Run `scripts/run-full-frame-x2.ps1` for the dynamic scenario. Use `-Scenes 0,1,2,3`
for the measured static controls; scene 4 is an additional mixed-cost fixture,
implemented but not part of the numbers above. `preview` mode can render the city
without running candidate exploration. `scripts/render-night-city-preview.py`
converts captured frames into a GIF/contact sheet.

The independent Python/NumPy evaluator recomputes timestamp intervals, image errors,
candidate decisions, best accepted candidate, workload consistency and identical
camera trajectories. It does not trust the renderer's speedup/quality summaries.
Targeted Release and Debug F/G tests pass, including isolated high-importance probe
admission and rejection of damage. Python validator adversarial tests pass.
The complete Release non-GPU/core/stress suite passed **45/45** after these changes.

[Byte-verified raw archive](https://github.com/Flyingzaptop/ARC/tree/results/night-city-20260919/results/night-city/20260919).
