# 00 — Project concept

## 1. Problem

Modern games are generally configured through static quality presets:

```text
Low / Medium / High / Ultra
```

Those presets are crude approximations. They are chosen before a scene is rendered and rarely account for:

- current VRAM pressure;
- current camera visibility;
- current per-resource usefulness;
- upcoming scene changes;
- RAM↔VRAM transfer speed;
- storage speed;
- real GPU headroom;
- dynamic thermal throttling;
- frame-generation cost/benefit;
- screen refresh target;
- temporal stability.

A machine may have 200 MB of currently useless free VRAM while simultaneously suffering from a compute bottleneck; another may be compute-idle while keeping several gigabytes of invisible textures resident. Static presets cannot continuously rebalance these resources.

ARC is intended to solve that class of problem.

## 2. Product thesis

Treat every available hardware resource as a budget and every possible quality/performance action as an investment.

Examples of actions:

- keep a 4K mip resident;
- drop a 4K mip but keep 2K/1K;
- evict an unseen resource to RAM;
- keep a decoded copy in RAM;
- prefetch an asset predicted to be needed soon;
- lower a safe shadow resource;
- increase internal render scale;
- reduce internal render scale;
- enable/disable temporal upscaling;
- enable/disable frame generation;
- change generated-frame ratio;
- reserve emergency VRAM headroom.

The system asks:

> Which combination maximizes perceived gaming quality while preserving stable frame delivery?

## 3. Unified optimization objective

Conceptually:

\[
U = w_qQ + w_mM + w_fF - w_lL - w_sS - w_aA - w_pP
\]

Where:

- `Q` = spatial image quality;
- `M` = motion quality;
- `F` = useful displayed frame rate;
- `L` = latency cost;
- `S` = stutter/frame-time instability;
- `A` = visual artifacts;
- `P` = catastrophic paging / resource miss penalty.

This is constrained by hardware budgets:

\[
VRAM \leq V_{safe}
\]

\[
RAM \leq R_{safe}
\]

\[
T_{frame} \leq T_{target}
\]

plus copy/storage/thermal constraints.

The real implementation does not need a single giant equation. It may use a hierarchy of controllers and hard safety limits. The equation is the design target.

## 4. Why textures come first

Textures are unusually attractive because they:

- often dominate VRAM usage;
- commonly contain mip chains;
- often allow graceful degradation;
- can often be partially resident;
- can be prioritized by visible screen contribution;
- can be prefetched;
- can be cached at multiple tiers.

A full mip chain is also heavily top-weighted. If the highest 4096² mip is removed and the resource starts effectively from 2048², roughly 75% of that texture chain's texel storage disappears. Starting from 1024² removes roughly 93.75% relative to the original top-level dominated chain, before format/alignment details.

This means a large fraction of apparent "PS5 needs 10 GB VRAM" demand can sometimes be transformed into:

```text
10 GB virtual graphics state
5 GB physically resident hot set
several GB RAM warm cache
remaining assets cold/on storage
```

without lying to the game's logical resource model.

## 5. Why ARC is broader than DLSS/FSR

Upscalers answer:

> How can I reconstruct a higher-resolution frame from a lower-resolution render?

Frame-generation systems answer:

> How can I synthesize intermediate display frames?

ARC asks a higher-level question:

> Is spending 200 MB and 1.5 ms on frame-generation buffers more valuable right now than spending the same memory on texture residency or shadow quality?

Thus DLSS/FSR-like technologies are potential **tools inside** ARC, not the entire product.

## 6. UX target

There should be no user-facing VRAM slider and no requirement to understand graphics APIs.

First run:

```text
Calibrating this PC...
```

Normal use:

```text
GAME
[ PLAY ]
```

Advanced diagnostic UI may exist for developers, but normal users should not be required to tune the governor.

## 7. Long-term scope

Potential clients:

- PS5 emulator;
- native D3D12 games;
- native Vulkan games;
- future console emulators;
- custom engines;
- eventually driver-integrated runtimes.

The first implementation must avoid hard-coding assumptions that only make sense for PS5.
