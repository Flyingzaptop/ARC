# Step 02 — Stage 2: Residency Governor

## Goal

Reduce memory pressure **without intentionally reducing image quality**.

At this stage ARC may change physical residency, but not logical content quality.

## Core idea

Classify resources by temperature:

```text
HOT
WARM
COLD
PINNED
UNKNOWN
```

Maintain a warm RAM cache where beneficial.

## Inputs

- OS VRAM budget;
- current usage;
- resource size;
- last use;
- reuse interval;
- transfer cost;
- queue dependencies;
- safety class.

## Actions

- keep resident;
- prefetch;
- evict;
- keep decoded RAM copy;
- keep compressed RAM copy;
- drop speculative prefetch.

## First algorithm

Start deterministic.

Example score:

\[
evict\_score =
coldness \times bytes\_saved
-
reload\_probability \times reload\_cost
\]

Do not overfit the formula; validate empirically.

## Emergency policy

When pressure crosses emergency threshold:

1. freeze promotions;
2. cancel prefetch;
3. evict cold safe resources;
4. increase emergency margin;
5. recover only after a cooldown.

## Metrics

- bytes evicted;
- bytes reloaded;
- false eviction rate;
- VRAM peak;
- P99 frame time;
- upload stalls.

## Exit criterion

ARC should demonstrate lower peak/pressure or fewer paging stalls in controlled tests without visual degradation and without increasing tail frame time.
