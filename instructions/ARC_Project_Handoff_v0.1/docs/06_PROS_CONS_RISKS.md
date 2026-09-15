# 06 — Pros, cons, risks

## Pros

### 1. Better use of fixed hardware

A static preset leaves resources idle or spends them inefficiently. ARC can rebalance continuously.

### 2. Low-VRAM machines become more viable

If textures dominate memory and high mips have low visible value, ARC may recover substantial VRAM while preserving perceived quality.

### 3. One optimizer can serve multiple frontends

A backend-neutral core can eventually support:

- emulator;
- D3D12;
- Vulkan;
- custom engines.

### 4. Graceful quality degradation

Instead of catastrophic OS paging:

```text
stable 60 FPS
→ slightly lower background texture detail
```

is preferable to:

```text
high texture setting
→ 200 ms stutter
```

### 5. Automatic temporal tradeoffs

ARC can decide whether frame generation/upscaling is worth their memory/compute/latency cost.

### 6. Hardware-specific optimization without hand-authored presets

Two laptops with the same nominal GPU may receive different policies based on measured capability.

---

## Cons

### 1. Enormous engineering complexity

ARC crosses:

- graphics APIs;
- memory management;
- render pipelines;
- shader behavior;
- OS budgets;
- frame pacing;
- temporal reconstruction;
- prediction.

### 2. Universal safe resource modification is impossible

Some textures are data. Some buffers contain opaque engine state. Some dimensions are assumed by shaders.

Therefore the safe default must be conservative.

### 3. Compatibility burden

Every API feature, engine behavior, and driver quirk can introduce edge cases.

### 4. Interception overhead

An observer/runtime can itself cause performance loss if poorly designed.

### 5. Anti-cheat and DRM compatibility

API interception may be rejected or blocked by protected games. ARC should not attempt bypass.

### 6. Temporal techniques add latency/artifacts

Generated frames improve displayed smoothness but are not equivalent to real simulation frames.

---

## Major technical risks

### Risk A — Observer changes timing too much

Mitigation:

- lock-free event path;
- batching;
- sampling modes;
- background compression;
- benchmark overhead continuously.

### Risk B — Wrong resource classification

Mitigation:

- UNKNOWN = do not modify;
- confidence thresholds;
- offline validation;
- rollback;
- capability-specific policies.

### Risk C — Quality oscillation

Mitigation:

- hysteresis;
- cooldowns;
- minimum hold durations;
- predictive promotion.

### Risk D — VRAM thrashing

Mitigation:

- warm RAM cache;
- residency cost model;
- reuse prediction;
- transfer cooldown.

### Risk E — "100% utilization" causes zero emergency room

Mitigation:

- adaptive headroom;
- scene volatility measurement;
- emergency mode.

### Risk F — Driver replacement consumes the entire project

Mitigation:

- keep custom-driver work isolated in research;
- ship API-level ARC first.

---

## Product risk

ARC may initially produce its largest wins only in workloads with:

- large texture footprints;
- predictable streaming;
- significant VRAM pressure.

That is acceptable. The project should prove value on favorable cases first rather than claiming universal gains immediately.
