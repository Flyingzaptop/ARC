# ML and control R&D

## Rule

Do not begin with reinforcement learning.

The first governor should be explainable and measurable.

## Good early uses of ML

- predict resource reuse;
- predict near-future VRAM demand;
- predict frame-time spike probability;
- estimate visual value of a resource;
- classify texture/data usage;
- estimate scene-transition probability.

## Bad early use

End-to-end agent directly calling:

```text
evict arbitrary buffer
resize arbitrary target
change arbitrary shader resource
```

without a safety layer.

## Controller design

Use:

```text
ML/statistics
→ predictions
→ deterministic safety filters
→ candidate-action optimizer
→ reversible action
```

## Offline training corpus

Eventually record anonymized/consented traces containing:

- resource sizes/types;
- timing;
- use sequences;
- memory pressure;
- frame time;
- actions;
- outcomes.

Avoid storing copyrighted game assets themselves when not required; resource metadata is usually enough for policy learning.

## Online adaptation

Use lightweight online updates:

- EWMA;
- Bayesian/statistical estimates;
- per-session reuse histograms.

Heavy training remains offline.
