# Step 03 — Stage 3: Adaptive Texture Governor

## Goal

Make the first deliberate quality trade:

> reduce texture memory where the visual cost is lowest.

## Preconditions

A texture may only enter automatic control when ARC has high confidence that:

- it is visual sampled data;
- mip reduction is semantically safe;
- required API representation supports the chosen action;
- it is not an opaque data texture.

## Initial actions

Prefer:

- prevent top mip from becoming resident;
- retain lower mips;
- sparse/tiled partial residency where available;
- RAM-cache removed mip data;
- reversible promotion later.

Avoid destructive re-encoding in the first version.

## Priority model

Useful factors:

- current visibility;
- projected screen size where obtainable;
- requested LOD/mip behavior;
- recent sampling frequency;
- camera/scene temporal stability;
- resource size;
- promotion cost;
- texture class confidence.

## Example

```text
Need 300 MB.

Candidate A: face mip0
save 40 MB
high visual loss

Candidate B: distant building mip0
save 110 MB
tiny loss

Candidate C: unseen terrain mip0
save 170 MB
near-zero current loss

Choose B + C.
```

## Anti-pop strategy

Quality promotion should happen before visibility when prediction confidence is high.

Use:

- promotion hysteresis;
- prefetch distance/time;
- minimum hold time;
- gradual priority changes.

## Exit criterion

Demonstrate meaningful VRAM reduction in controlled texture-heavy workloads while preserving frame pacing and keeping visible degradation below defined thresholds.
