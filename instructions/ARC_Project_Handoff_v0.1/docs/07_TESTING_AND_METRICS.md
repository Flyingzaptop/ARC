# 07 — Testing and metrics

## 1. Measure what users feel

Do not optimize average FPS alone.

Primary metrics:

- median frame time;
- P95/P99/P99.9 frame time;
- 1% low;
- 0.1% low;
- missed presentation;
- frame pacing variance;
- input/display latency proxy;
- VRAM pressure;
- bytes uploaded/evicted per second;
- resource miss rate;
- texture promotion latency;
- artifact rate.

## 2. Synthetic workloads

Create controlled samples.

### texture_pressure

- 512²–8192² textures;
- multiple compressed formats;
- controllable mip usage;
- predictable visibility path.

### residency_pressure

- gradually cross OS budget;
- observe paging/stutter;
- compare ARC residency controller.

### streaming_city

- moving camera;
- known future visibility;
- known correct prefetch set.

### compute_pressure

- heavy compute load;
- test whether ARC correctly avoids enabling costly temporal workloads.

### temporal_test

- known motion vectors/depth;
- evaluate FG/upscaling cost and artifacts.

## 3. Ground truth

Synthetic tests must know the correct answers:

- exact allocated bytes;
- exact lifetimes;
- exact mip use;
- exact queue activity;
- exact visible-resource set.

Observer results are compared automatically.

## 4. Stage-1 targets

Suggested targets:

- >99.9% resource create/destroy event capture in synthetic tests;
- memory accounting error under 3%;
- no graph corruption in long tests;
- no output modification in observer mode;
- production observer CPU overhead preferably under 2%;
- steady-state frame-time overhead preferably under ~0.2 ms on target hardware.

Targets are engineering goals, not marketing guarantees.

## 5. Governor A/B methodology

Every optimization test should compare:

```text
BASELINE
vs
ARC
```

using identical:

- executable;
- scene path;
- resolution;
- save state where possible;
- driver;
- system power mode.

Report:

```text
VRAM
RAM
average FPS
1% low
0.1% low
P99 frametime
transfer bandwidth
visible quality metric
artifact count
```

## 6. Quality assessment

Eventually combine:

- SSIM/PSNR for controlled outputs;
- LPIPS-like perceptual metrics offline;
- motion-aware error;
- human review;
- resource-specific quality heuristics.

ARC's optimization target is perceived quality, so purely numeric texture-resolution scoring is insufficient.

## 7. Regression suite

Every supported optimization action gets a regression set.

Examples:

```text
drop-top-mip
sparse-residency
RAM-warm-cache
prefetch
framegen-enable
render-scale-change
```

A new optimization cannot merge if it regresses correctness or increases tail frame time beyond threshold.
