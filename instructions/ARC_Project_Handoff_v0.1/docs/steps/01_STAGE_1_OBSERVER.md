# Step 01 — Stage 1: ARC Observer

## Goal

Build a read-only graphics observer that reconstructs the game's GPU resource behavior without changing rendered output.

This stage is the foundation of everything else.

## Deliverables

- `arc-core`;
- `arc-calibrate`;
- `arc-dx12-observer`;
- `dx12-memory-pressure`;
- binary trace format;
- resource graph;
- developer trace viewer;
- synthetic ground-truth validation.

## Implementation sequence

### 1. Core primitives

Implement:

- `ResourceId`;
- `HeapId`;
- `DescriptorId`;
- `QueueId`;
- `FrameId`;
- monotonic timestamps;
- event sequence IDs.

### 2. Fast event path

Render/API threads write compact events into per-thread buffers.

Do not:

- allocate dynamically on every event;
- format strings;
- lock a global mutex;
- write files synchronously.

### 3. D3D12 observation surface

Start with:

- adapter/device creation;
- heaps;
- committed/placed/reserved resources;
- descriptor heaps/views;
- command queues;
- command lists;
- barriers;
- copies;
- draw/dispatch counters;
- fences;
- swapchain/present.

### 4. Resource lifetime model

Every resource has:

```text
create
bind/view relationships
reads/writes
copies
last use
destroy
```

### 5. Frame segmentation

Use presentation as initial frame boundaries, but store queue timeline independently because frames-in-flight break simplistic assumptions.

### 6. Budget capture

Record OS-reported video-memory budget and usage periodically and on relevant events.

### 7. Trace persistence

Suggested:

```text
session.json
events.arcbin
resources.sqlite
```

The binary trace stores high-frequency events.

### 8. Viewer

Minimum views:

- frame timeline;
- VRAM budget/usage;
- resource table;
- resource detail;
- queue activity;
- largest resources;
- cold resources.

## Exit criteria

Do not proceed until:

- synthetic byte accounting is accurate;
- create/destroy is nearly complete;
- long runs do not corrupt graph state;
- observer overhead is small;
- output is unchanged.
