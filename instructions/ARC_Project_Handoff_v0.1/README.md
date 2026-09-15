# ARC — Adaptive Runtime Core
## Complete engineering handoff v0.1

ARC is a universal autonomous game-performance runtime whose job is to turn the hardware actually available on a PC into the best possible gaming result without asking the user to tune graphics settings manually.

The user-facing target is intentionally simple:

```text
SELECT GAME
PLAY
```

ARC decides what should live in VRAM, RAM, or storage; what can be streamed, evicted, prefetched, downgraded, reconstructed, upscaled, or temporally synthesized; and how much headroom must remain to avoid stutter.

The first high-control client is planned to be a PS5-emulation backend, because an emulator naturally sits between guest GPU semantics and the host GPU API. The ARC core, however, must be platform-neutral enough to later support native Windows games through D3D12/Vulkan adapters.

## Core product principle

ARC does **not** optimize "settings".

ARC optimizes the **perceived output of the whole machine**:

- image quality;
- motion quality;
- frame rate;
- frame pacing;
- input/display latency;
- stutter probability;
- artifact rate;
- loading/streaming behavior.

Subject to:

- GPU compute/raster budget;
- CPU budget;
- VRAM budget;
- RAM budget;
- PCIe/copy budget;
- storage budget;
- thermal/power behavior;
- API/engine safety constraints.

## Non-goals for v0.x

ARC v0.x does not:

- bypass DRM;
- bypass anti-cheat;
- bypass driver signing or kernel security;
- patch arbitrary game logic;
- promise safe modification of unknown GPU data structures;
- replace the NVIDIA/AMD Windows display driver.

## Package map

Read in this order:

1. `docs/00_PROJECT_CONCEPT.md`
2. `docs/01_ARCHITECTURE.md`
3. `docs/02_OPTIMIZATION_MODEL.md`
4. `docs/03_RESOURCE_MODEL.md`
5. `docs/04_HARDWARE_CALIBRATION.md`
6. `docs/05_PS5_BACKEND_CONCEPT.md`
7. `docs/06_PROS_CONS_RISKS.md`
8. `docs/07_TESTING_AND_METRICS.md`
9. `docs/08_REPOSITORY_AND_BUILD_PLAN.md`
10. `docs/09_SECURITY_LEGAL_COMPATIBILITY.md`
11. `docs/steps/*` in numeric order
12. `docs/research/*`

## Most important development rule

Every capability follows this sequence:

```text
OBSERVE
  ↓
UNDERSTAND
  ↓
CLASSIFY
  ↓
PREDICT
  ↓
ACT
  ↓
MEASURE
  ↓
ROLL BACK IF WORSE
```

Never:

```text
GUESS → CHANGE → HOPE
```

That rule is the difference between a stable optimization runtime and a graphics injector that occasionally breaks games.
