# Step 04 — Stage 4: Predictive Streaming

## Goal

Move data before it becomes urgently needed.

## Start without ML

Use statistics:

- reuse intervals;
- recency/frequency;
- resource co-usage;
- scene/descriptor transitions;
- queue history;
- sequential streaming patterns.

Estimate:

\[
P(resource\ used\ in\ next\ \Delta t)
\]

for horizons such as:

```text
50 ms
100 ms
250 ms
500 ms
1000 ms
```

## Prefetch decision

Prefetch if:

```text
probability × miss_penalty
>
transfer_cost + opportunity_cost
```

## RAM tiers

Support:

```text
VRAM hot
RAM decoded warm
RAM compressed warm
storage cold
```

ARC chooses which tier is worth maintaining.

## Evaluation

Track:

- prediction precision;
- prediction recall;
- bytes prefetched but unused;
- late resource misses;
- transfer spikes;
- visible pop-in;
- P99 frame time.

## ML later

Only after statistical baseline is strong.

Potential later model inputs:

- resource sequence;
- frame features;
- scene changes;
- historic demand;
- camera proxies.

The ML model should output probabilities, not direct unsafe memory operations.
