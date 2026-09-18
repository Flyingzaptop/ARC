# Mega C / Stage 15 — Automatic Scene Understanding

Stage 15 begins from the accepted Stage 14.5 Wicked Engine truth test.

## Safety boundary

The first Stage 15 implementation is observer-only. Inferred semantics are not fed into
quality mutation or residency control until their behavior is measured against an
external truth set.

## Behavioral resource semantics

ARC classifies resources using only backend-neutral runtime evidence already present
in the ResourceGraph:

- resource kind and dimensions;
- mip/layer/sample structure;
- SRV/UAV/RTV/DSV/CBV evidence;
- read/write balance;
- usage count and burstiness;
- inter-frame reuse interval;
- queue fan-out and resource age.

The controller is not given engine object names, pass names, scene names, or manually
assigned semantic tags.

Current output classes:

- Unknown
- MaterialTexture
- RenderTarget
- DepthBuffer
- ShadowMap
- StorageTexture
- GeometryBuffer
- UploadLikeBuffer
- ReadbackLikeBuffer
- TransientIntermediate
- PersistentHistory

Each prediction carries a confidence and evidence mask so downstream code can fail
closed on low-confidence or ambiguous resources.

## Phase A acceptance

Phase A is complete when:

1. deterministic semantic tests are green on Linux and Windows;
2. no regression to Mega A/B/Stage 14.5 tests;
3. the classifier can run over a complex Wicked trace without ResourceGraph errors;
4. unknown/low-confidence resources remain untouched;
5. semantic predictions are stable enough to support a separate truth-label study.

Stage 15 is not closed by Phase A. Engine-independent truth-label accuracy and
scene/pass understanding remain required before inferred semantics may affect the
governor.
