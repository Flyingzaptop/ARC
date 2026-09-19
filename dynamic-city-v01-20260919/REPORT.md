# Dynamic City: baseline vs current v0.1

Identical closed camera route, actors and settings; clean processes without ARC for both baselines. No full-frame readback during timing. The v0.1 run uses the actual generic DX12 VRS command-substitution DLL in lean mode.

| Run | Frames | Seconds | FPS | p99 frame ms | 1% low FPS |
|---|---:|---:|---:|---:|---:|
| baseline-a | 13500 | 15.964 | 845.64 | 2.934 | 292.88 |
| v01 | 15300 | 15.608 | 980.25 | 3.537 | 250.84 |
| baseline-b | 12600 | 15.520 | 811.84 | 3.255 | 252.96 |

Combined baseline: **828.40 FPS**; v0.1: **980.25 FPS**; change **+18.33%**. Baseline repeat drift: 4.16%.

Actual substitution coverage including warmup/captures: 16621/16639; 18 bounded-capacity fallbacks used the original command list. These fallbacks remain included in the measured result.

| Component | Baseline ms | v0.1 ms |
|---|---:|---:|
| Карта теней | 0.0497 | 0.0501 |
| Геометрия + свет | 0.3525 | 0.1427 |
| Объёмный свет (compute) | 0.3629 | 0.3464 |
| Тонмаппинг + bloom | 0.1124 | 0.0665 |
| Total measured GPU | 0.8775 | 0.6058 |
| CPU recording | 0.0945 | 0.1368 |
| CPU submission | 0.0476 | 0.0597 |
| GPU fence wait | 0.9601 | 0.7180 |
| Timing readback CPU | 0.0034 | 0.0036 |
| CPU Present | 0.0969 | 0.0975 |

Inter-frame GPU-timestamp gap: 0.3296 → 0.4144 ms. This includes queue gaps and work outside the marked render passes; it is not identified solely as CPU or driver time.

Fog/volumetric compute contributes 57.2% of the remaining marked GPU work. VRS acts on raster pixel shading, so this compute pass is unchanged in mechanism.

Image guard: **FAIL** across 12 matched poses. Full-quality references identical: 12/12. Worst mean/peak/tile linear error: 0.006716 / 0.928974 / 0.304444.

This is a lightweight instanced synthetic scene, not a game-speedup result. Geometry consists of 1,294 procedural cuboid instances; there is no DXR, streaming, game simulation or engine overhead. The camera follows a fixed 15-second simulation cycle, replayed without a wall-time FPS cap. All complete cycles have the same pose distribution. The preview GIF is a sparse accelerated route overview.

Next: address the measured compute-lighting cost; use conservative quality-aware controls and retain independent reference/rollback checks. Reduce command-substitution overhead and preserve fine edges before accepting a whole-frame VRS change. Add meaningful geometry/material/streaming complexity without inflating baseline work merely to manufacture a speedup.

## Presentation-path diagnostic

Using three ordinary offscreen render targets instead of the presentation path: 1072.12 → 1558.52 rendering FPS (+45.4%). These are throughput diagnostics and are not the main displayed-window comparison.

The inter-frame GPU gap drops to 0.0166 / 0.0162 ms. Corresponding screenshots match 12/12 baseline and 12/12 v0.1 images. Thus the presentation path is a material limiter in this windowed benchmark, in addition to volumetric compute. The diagnostic does not isolate the OS compositor, driver and API costs from one another.
