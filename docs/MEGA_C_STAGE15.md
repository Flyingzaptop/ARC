# Mega C / Stage 15 — Automatic Scene Understanding

Stage 15 starts from the accepted Stage 14.5 Wicked Engine baseline at `17af73117827c67cc7304bf19e66d3486f1caab3`.

## Safety boundary

Stage 15 remains observer-only until its real Wicked GPU truth gate passes. Resource and scene semantics are not used by the governor, residency planner, or quality mutation path. Wicked scene names are evaluation metadata only.

The runtime inference path receives no engine object names, pass names, scene names, or manually assigned semantic tags.

## Layer 1 — behavioral resource semantics

`ResourceSemanticInferencer` classifies resources from backend-neutral runtime evidence already stored in `ResourceGraph`:

- resource kind, dimensions, allocation size, mip/layer/sample structure;
- SRV/UAV/RTV/DSV/CBV evidence;
- read/write balance;
- usage count, burstiness, inter-frame reuse;
- queue fan-out and resource age.

Predictions contain a semantic class, confidence, evidence mask, and the feature vector used to make the decision. The feature-vector overload allows scene windows to be classified from per-window activity rather than lifetime counters.

## Layer 2 — scene signatures

`SceneUnderstandingInferencer` snapshots resource counters at scene-window entry. At the end of the window it builds a label-free signature from only activity that occurred after that checkpoint:

- active and semantically known resource populations;
- active and known byte populations;
- resource-semantic and byte-semantic distributions;
- read/write balance;
- multi-queue participation;
- semantic coverage and mean semantic confidence.

The checkpoint snapshots read/write counts, burst counters, per-queue use counters, and reuse-gap aggregates. Scene-window features are derived from counter deltas, so queue fan-out and reuse evidence from an earlier scene cannot leak into the next scene signature.

A convenience rolling-window path exists for offline inspection; the Wicked acceptance path uses explicit checkpoints. Online cluster assignments are required to remain finite/JSON-safe even when the first observation creates a new cluster.

## Similarity and online clustering

Scene comparison combines total-variation distance over resource and byte semantic distributions with behavioral and logarithmic population distances. The default same-scene and cluster thresholds are 0.22. Empty or completely unknown signatures fail closed.

`SceneSemanticClusterer` assigns label-free online cluster IDs and updates centroids conservatively. Cluster IDs have no engine-specific meaning.

## Wicked truth protocol

The Wicked bridge captures one scene signature at the end of each baseline and adaptive scene slot. The bridge stores captures by slot for evaluation bookkeeping, but the scene index/name is never passed into the inferencer or clusterer.

The result JSON contains resource-semantic coverage, baseline/adaptive scene signatures, label-free cluster assignments, and a full baseline-to-adaptive distance matrix. It explicitly records `truth_labels_used_for_inference=false` and `semantic_labels_used_by_controller=false`.

Only `stage15-wicked-bootstrap.ps1`, after the run has completed, compares the matrix and cluster IDs with external Wicked truth labels.

## Stage 15 GPU acceptance

A real Wicked GPU run closes Stage 15 only when all Stage 14.5 gates remain green and:

1. all baseline/adaptive signatures are captured and non-empty;
2. resource semantic coverage and confidence remain useful;
3. nearest-baseline retrieval is correct for at least 80% of adaptive scenes;
4. at least 80% of true pairs are inside the same-scene threshold;
5. at least 80% of true pairs have positive margin against the best wrong scene;
6. mean identity margin is at least 0.02;
7. at least 80% of true baseline/adaptive pairs retain the same online cluster ID;
8. clustering is non-trivial (at least two clusters);
9. truth labels remain absent from inference/controller inputs;
10. ResourceGraph and quality backend remain clean.

## Verification status

CPU implementation is covered by deterministic scene-understanding tests for semantic separation, rolling activity, checkpoint isolation, fail-closed empty windows, and online cluster recurrence.

Repository CI must additionally pass Linux Release, Windows Debug/Release, PowerShell parsing, the pinned external renderer build check, and the pinned Wicked Stage 14.5 integration build check. The hardware bootstrap explicitly runs `arc-resource-semantics-tests` and `arc-scene-understanding-tests` before preparing Wicked.

The final real GPU Wicked truth run is intentionally external to CI and remains the only hardware-dependent Stage 15 gate. A completed run is published immutably under a dedicated `results/stage15-wicked-<timestamp>` branch with artifacts stored below `results/stage15-wicked/<timestamp>`.
