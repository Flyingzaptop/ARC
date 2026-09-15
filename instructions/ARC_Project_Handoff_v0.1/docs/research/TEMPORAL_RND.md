# Temporal rendering R&D

## Scope

Investigate:

- temporal upscaling;
- optical flow;
- frame interpolation;
- frame generation;
- adaptive presentation pacing.

## Core integration principle

Temporal rendering is a resource consumer like any other subsystem.

It spends:

- VRAM;
- GPU compute;
- bandwidth;
- latency.

Therefore it must compete with other ARC actions.

## Example

If 180 MB free VRAM can either:

- keep a barely visible 4K texture mip, or
- enable a high-value temporal feature,

ARC should choose based on measured output utility.

## Real vs generated frames

Never confuse displayed frame rate with simulation frame rate.

ARC telemetry must separately report:

```text
real FPS
generated FPS
displayed FPS
```

## Latency guard

If input/display latency worsens beyond policy threshold, temporal gain is rejected even if displayed FPS rises.
