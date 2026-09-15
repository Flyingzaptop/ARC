# Step 06 — Stage 6: Temporal upscaling and frame generation

## Goal

Allow ARC to trade spatial render cost, temporal reconstruction cost, latency, and memory dynamically.

## Upscaling

ARC may choose lower internal resolution when:

- GPU is raster/compute limited;
- reconstruction quality is acceptable;
- saved GPU time has a better use.

The saved budget may be spent on:

- higher real frame rate;
- better textures;
- better shadows;
- temporal frame synthesis;
- emergency reserve.

## Frame generation

Frame generation is not "free FPS".

Cost dimensions:

- intermediate buffers;
- motion/depth/optical-flow inputs;
- compute time;
- presentation complexity;
- latency;
- artifacts.

ARC must enable it only when the utility gain exceeds those costs.

## Adaptive generated-frame ratio

Long-term concept:

```text
real frames are the simulation anchor
generated frames fill display cadence
```

For a 165 Hz display, ARC may target a display cadence rather than a fixed "2x" label.

However, high generated-to-real ratios should only be explored after quality/latency validation. One generated frame between real frames is the safest initial target.

## Safety

If motion/depth data is not trustworthy or if generated artifacts spike, ARC disables FG automatically.

## Metrics

- displayed FPS;
- real FPS;
- generated FPS;
- end-to-end latency proxy;
- artifact/error metric;
- GPU compute cost;
- VRAM cost;
- frame pacing.
