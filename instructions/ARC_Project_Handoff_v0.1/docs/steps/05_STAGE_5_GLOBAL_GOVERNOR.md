# Step 05 — Stage 5: Global Quality Governor

## Goal

Unify memory and rendering decisions.

At this stage ARC stops thinking in terms of "texture manager" and starts optimizing the whole rendering budget.

## Candidate domains

- texture residency;
- shadow resources where safely controllable;
- reflection/environment resources;
- selected transient targets;
- geometry LOD where integration proves safe;
- internal render scale where integration supports it;
- temporal upscaling;
- frame generation.

## Global budget vector

Represent current hardware state as:

```text
VRAM free/safe
RAM free/safe
GPU graphics headroom
GPU compute headroom
CPU headroom
copy headroom
storage headroom
latency margin
frame-time margin
```

## Action market

Every controller submits candidate actions.

The governor resolves conflicts.

Example:

```text
200 MB free VRAM.

Texture controller:
+ 4K background mip
benefit 0.2

Temporal controller:
+ FG buffers
benefit 3.0
compute cost 1.3 ms
latency cost 0.5

Shadow controller:
+ higher shadow cache
benefit 0.6

Global governor chooses based on total utility and constraints.
```

## Important

Never optimize toward 100% GPU if it destroys spike tolerance.

The target is maximum useful output with adaptive reserve.
