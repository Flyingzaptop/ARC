# START HERE

If another engineer/model receives this ZIP with zero prior context:

1. Read `README.md`.
2. Read `docs/00_PROJECT_CONCEPT.md`.
3. Read `docs/01_ARCHITECTURE.md`.
4. Read `docs/steps/01_STAGE_1_OBSERVER.md`.
5. Read `docs/steps/01A_STAGE_1_DETAILED_IMPLEMENTATION.md`.
6. Read `docs/11_FIRST_30_COMMITS.md`.
7. Implement only Stage 1.
8. Do not build the custom driver, PS5 emulator, ML governor, or frame-generation integration before the Observer passes its completion gate.

Immediate code targets:

```text
arc-core
arc-calibrate
arc-dx12-observer
dx12-memory-pressure
arc-trace-viewer
```

Immediate success condition:

> ARC can reconstruct a controlled D3D12 workload's resource/memory behavior accurately, with very low overhead, while changing no visible output.
