# 01 — Architecture

## 1. Top-level system

```text
                    GAME / EMULATOR
                          │
                          ▼
                  API / GUEST ADAPTER
                          │
          ┌───────────────┴────────────────┐
          │       ARC Runtime Core         │
          │                                │
          │  Hardware Model                │
          │  Resource Graph                │
          │  Telemetry                     │
          │  Classifier                    │
          │  Predictor                     │
          │  Residency Manager             │
          │  Quality Manager               │
          │  Temporal Manager              │
          │  Global Governor               │
          │  Safety / Rollback             │
          └───────────────┬────────────────┘
                          │
                D3D12 / Vulkan backend
                          │
                          ▼
                  Vendor GPU Driver
                          │
                          ▼
                         GPU
```

## 2. Module boundaries

### ARC Core

Backend-neutral logic:

- resource IDs;
- resource graph;
- temporal usage history;
- budget abstraction;
- policy engine;
- safety classification;
- predictive scoring;
- trace/event model;
- governor decisions.

Core code must not depend on `ID3D12Resource*` or `VkImage` directly.

### D3D12 adapter

Responsibilities:

- observe resource creation;
- observe heaps;
- observe descriptors;
- observe barriers;
- observe copy operations;
- observe command submission;
- observe presentation;
- expose DXGI memory budget;
- later execute residency operations where safe.

### Vulkan adapter

Responsibilities equivalent to D3D12, implemented as an explicit Vulkan layer where practical.

### PS5 adapter

Translates guest GPU/OS resource semantics into ARC's virtual resource model before host allocation.

This is potentially the most powerful frontend because ARC can know the guest resource intention before a physical host resource exists.

## 3. Fast path vs slow path

ARC must never put expensive analysis directly in hot API-call paths.

### Fast path

Runs on render/API threads.

It may only:

- allocate stable IDs;
- append compact events;
- update tiny atomic counters;
- consult precomputed decisions;
- enforce already-approved residency/quality actions.

### Slow path

Runs asynchronously.

It performs:

- graph analysis;
- prediction;
- scoring;
- policy optimization;
- logging;
- compression;
- trace persistence;
- statistics;
- model updates.

## 4. Controller hierarchy

A single monolithic optimizer is risky. Use hierarchy:

```text
Global Governor
│
├── Safety Controller
├── Frame-Time Controller
├── Memory Pressure Controller
├── Residency Controller
├── Texture Quality Controller
├── Render Quality Controller
└── Temporal Controller
```

Safety has veto power.

Example:

```text
Temporal Controller:
"Enable frame generation"

Safety Controller:
"Rejected: insufficient motion/depth data"
```

## 5. Hard safety boundaries

Never modify an unknown resource merely because it "looks large".

Every optimization action must have:

- a capability flag;
- an evidence score;
- a reversibility level;
- expected benefit;
- expected cost;
- risk score.

Unknown defaults to no modification.

## 6. Headroom model

Do not chase literal 100% utilization.

ARC should instead target:

```text
high useful utilization
+ small adaptive emergency reserve
```

The emergency reserve varies by workload volatility.

A stable menu might allow near-total resource use.

A streaming open-world scene needs larger reserve.

## 7. Failure behavior

ARC should fail gracefully.

Preferred behavior:

```text
optimization module failure
        ↓
disable optimization
        ↓
continue normal rendering
```

Never:

```text
optimization error
        ↓
corrupt game resource
```

This fail-open design is especially important before ARC gains broad compatibility.
