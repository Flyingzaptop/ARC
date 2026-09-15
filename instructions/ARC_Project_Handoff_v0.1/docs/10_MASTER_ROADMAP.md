# 10 — Master roadmap

## Milestone M0 — Repository exists

Output:

- buildable C++ project;
- CI;
- coding conventions;
- basic tests.

## M1 — Calibration

Output:

- hardware profile;
- dynamic VRAM-budget reader;
- transfer/compute/storage measurements.

## M2 — D3D12 Observer

Output:

- trace;
- resource graph;
- memory accounting;
- frame timeline.

## M3 — Observer validation

Output:

- synthetic test suite;
- ground-truth comparison;
- overhead report.

## M4 — Residency Governor

Output:

- safe prefetch/eviction;
- warm RAM cache;
- emergency pressure handling.

## M5 — Texture Governor

Output:

- safe adaptive mip residency;
- reversible promotions;
- measurable VRAM savings.

## M6 — Predictive Streaming

Output:

- future-use scoring;
- lower miss rate;
- reduced pop/stall.

## M7 — Global Governor

Output:

- unified action scoring;
- cross-domain tradeoffs.

## M8 — Temporal Module

Output:

- upscaling integration;
- frame-generation integration;
- latency-aware enable/disable.

## M9 — PS5 backend integration

Output:

- ARC virtual resources in emulator path;
- guest-aware resource classification;
- low-VRAM optimization.

## M10 — Native Vulkan/D3D12 game pilots

Output:

- selected unprotected titles;
- compatibility matrix;
- real-world benchmark report.

## M11 — Driver-assisted R&D

Output:

- AMD/Linux prototype;
- measured comparison against user-space mode.

## The order is deliberate

Do not skip directly to:

```text
"AI optimizer"
```

or:

```text
"custom GPU driver"
```

before M2–M5 prove that the central resource model actually works.
