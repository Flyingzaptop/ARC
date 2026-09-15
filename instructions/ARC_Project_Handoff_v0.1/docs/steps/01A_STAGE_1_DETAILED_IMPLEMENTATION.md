# Stage 1 — Detailed implementation specification
## ARC Observer + Calibration + Ground-Truth Harness

This document is intentionally implementation-heavy. It is the document to hand to an engineer and say: **build this first**.

---

# 1. Stage-1 objective

Stage 1 must answer, with high confidence:

1. What GPU resources exist?
2. How much memory do they consume?
3. Which API objects reference them?
4. When are they created and destroyed?
5. Which queues use them?
6. When are they read, written, copied, or transitioned?
7. What is the OS-reported memory budget at that moment?
8. Which resources appear visual vs opaque data?
9. Which resources are hot, warm, or cold over time?
10. What is the overhead of observing all of this?

Stage 1 **must not deliberately change rendering behavior**.

The invariant is:

```text
same application
same visible output
same logical GPU work
+ ARC observation
```

within measurement noise.

---

# 2. Stage-1 binaries

Build these targets:

```text
arc-core
arc-calibrate
arc-trace
arc-resource-graph
arc-dx12-observer
arc-trace-viewer

dx12-memory-pressure
dx12-texture-pressure
dx12-mixed-resources
```

Optional later in Stage 1:

```text
arc-vulkan-observer
vk-memory-pressure
```

Do not start Vulkan until the backend-neutral event/resource model is stable enough that Vulkan maps cleanly into it.

---

# 3. arc-core responsibilities

`arc-core` owns no D3D12 COM object directly.

It defines:

```cpp
namespace arc {

using ResourceId   = uint64_t;
using HeapId       = uint64_t;
using DescriptorId = uint64_t;
using QueueId      = uint64_t;
using CommandId    = uint64_t;
using FrameId      = uint64_t;
using SessionId    = uint64_t;

}
```

IDs must:

- never depend on raw pointer values;
- not be reused in a session;
- be cheap to allocate;
- be serializable;
- remain backend-neutral.

Use a monotonic atomic counter per ID domain or one globally unique counter if simpler.

---

# 4. Time model

Use a monotonic high-resolution clock.

Every trace event stores:

```cpp
uint64_t timestamp_ns;
uint64_t sequence;
uint32_t thread_id;
```

`sequence` resolves ordering when timestamps collide or clock resolution is insufficient.

Do not use wall-clock time for internal ordering.

Session metadata may store wall-clock start time separately.

---

# 5. Event transport

The event path is performance critical.

Forbidden in the graphics hot path:

- file I/O;
- JSON serialization;
- formatted logging;
- global blocking mutex per event;
- heap allocation per event.

Preferred pipeline:

```text
API interception thread
        ↓
thread-local / lock-free event buffer
        ↓
collector thread
        ↓
batch
        ↓
optional compression
        ↓
trace file
```

The event writer must expose:

```cpp
bool try_emit(Event e) noexcept;
```

If the buffer overflows, Stage 1 must:

- increment an overflow counter;
- mark the trace as incomplete;
- never block the render thread indefinitely.

Correctness testing should size buffers so overflow does not occur.

Production observation may prefer dropping low-value events over stalling a game.

---

# 6. Event schema

Minimum events:

```text
SessionStart
SessionEnd

AdapterObserved
DeviceCreated

HeapCreated
HeapDestroyed

ResourceCreated
ResourceDestroyed

DescriptorHeapCreated
DescriptorWritten

CommandQueueCreated
CommandListCreated
CommandListReset
CommandListClosed

Barrier
CopyResource
CopyBuffer
CopyTexture

Draw
DrawIndexed
Dispatch
ExecuteIndirect

QueueSubmit
FenceSignal
FenceWait

SwapchainCreated
Present

MemoryBudgetSample
TelemetrySample

TraceOverflow
DiagnosticError
```

High-frequency draw events may initially carry only counters rather than full state snapshots.

---

# 7. Resource creation payload

Normalize backend details into something like:

```cpp
enum class ResourceKind : uint8_t {
    Buffer,
    Texture1D,
    Texture2D,
    Texture3D,
    Unknown
};

struct ResourceCreateEvent {
    ResourceId id;
    HeapId heap;

    ResourceKind kind;

    uint64_t virtual_bytes;
    uint64_t allocation_bytes;

    uint32_t width;
    uint32_t height;
    uint32_t depth;

    uint16_t mip_levels;
    uint16_t array_layers;

    ArcFormat format;
    ArcUsageFlags usage;

    uint64_t backend_flags;
};
```

Keep original backend flags somewhere for debugging, but the core graph should use normalized semantics.

---

# 8. Allocation-size accuracy

Texture byte estimation is not always equal to:

```text
width × height × bytes_per_pixel
```

because of:

- block compression;
- mip chains;
- alignment;
- tiling;
- MSAA;
- allocation granularity;
- placed-resource heap packing.

Therefore store both:

```text
logical payload estimate
host allocation footprint estimate
```

For D3D12 use device allocation-info queries where appropriate in the observer/sample path.

Do not pretend the estimate is exact if the API/driver does not expose an exact physical byte count.

---

# 9. Descriptor/view tracking

Stage 1 must map view objects to resources.

Normalized relationship:

```text
DescriptorId
    ↓
ViewType
    ↓
ResourceId
    ↓
SubresourceRange
```

View types:

```text
SRV
UAV
RTV
DSV
CBV
Sampler
Unknown
```

Suggested structure:

```cpp
struct ViewRecord {
    DescriptorId id;
    ResourceId resource;
    ViewType type;

    uint16_t first_mip;
    uint16_t mip_count;

    uint16_t first_layer;
    uint16_t layer_count;

    ArcFormat view_format;
};
```

This matters because:

```text
Texture exists
```

is much less useful than:

```text
Texture is exposed as SRV mips 2..8
```

---

# 10. Resource-state timeline

For each resource maintain a temporal log or summarized state transitions.

Do not store an unbounded vector inside every live resource in hot memory if that becomes expensive.

Possible approach:

- current state in memory;
- transition events in trace;
- compact rolling statistics in resource record.

Track at least:

```text
last_read
last_write
last_copy_in
last_copy_out
last_transition
last_queue
```

---

# 11. Queue model

Normalize queue types:

```cpp
enum class QueueClass : uint8_t {
    Graphics,
    Compute,
    Copy,
    Unknown
};
```

Track:

- queue creation;
- command list submission;
- fence signals;
- fence waits;
- CPU waits if observable;
- present relationship.

Later ARC needs to know whether a transfer can overlap rendering.

Stage 1 therefore must not collapse all GPU work into a single timeline.

---

# 12. Frame model

`Present` creates a useful user-visible cadence marker, but it is not a perfect definition of GPU frames because of frames-in-flight.

Maintain two concepts:

```text
PresentationFrameId
SubmissionEpoch
```

Initial implementation may associate events with nearest presentation interval while retaining raw queue timestamps so later analysis can reconstruct overlap.

Never throw away the queue timeline.

---

# 13. Memory-budget sampling

For D3D12/DXGI capture:

```text
Budget
CurrentUsage
AvailableForReservation
CurrentReservation
```

Store samples:

```cpp
struct MemoryBudgetSample {
    uint64_t timestamp_ns;

    uint64_t local_budget;
    uint64_t local_usage;
    uint64_t local_available_for_reservation;
    uint64_t local_current_reservation;

    uint64_t nonlocal_budget;
    uint64_t nonlocal_usage;
};
```

Sample:

- periodically;
- at session start;
- after large allocations;
- when notified of budget changes if using OS notification.

Do not assume physical VRAM equals budget.

---

# 14. Hardware calibration executable

`arc-calibrate` outputs a versioned `HardwareProfile`.

## Tests

### CPU

- single-thread integer/floating proxy;
- multithread scaling proxy;
- memory copy;
- decompression benchmark using representative codec(s).

### RAM

- large sequential copy;
- multi-thread copy;
- latency-ish pointer chase;
- available capacity.

### GPU

Use controlled ARC sample workloads:

- upload throughput;
- readback throughput;
- copy-queue throughput;
- compute proxy;
- graphics proxy;
- queue-overlap test.

### Storage

On selected game volume:

- large sequential read;
- medium/random reads;
- cold-ish and warm-cache distinction where practical.

Do not run destructive disk benchmarks.

---

# 15. Calibration statistics

Never store only one result.

For each benchmark:

```text
warmup
N measured runs
median
P10
P90
variance
```

If results vary heavily, mark confidence lower.

A laptop that throttles halfway through calibration must not receive the same profile as one that sustains clocks.

---

# 16. Hardware profile format

Required fields:

```text
schema version
OS build
ARC build
GPU vendor/device
GPU driver
physical VRAM
observed safe budget range
RAM capacity
storage volume
display refresh/resolution
benchmark medians
benchmark variance
timestamp
```

The profile is a starting prior, not a permanent truth.

Runtime telemetry can revise estimated costs.

---

# 17. Resource Graph v0

Create a backend-neutral graph.

Node classes:

```text
Resource
Heap
View/Descriptor
Queue
Pipeline (optional early)
Frame/Submission epoch
```

Edge classes:

```text
allocated-in
view-of
read-by
written-by
copied-to
used-on-queue
alive-during
```

The graph must support queries:

```text
largest textures now
resources unused for N frames
resources used by copy queue recently
resources with SRV but no UAV
resources with render-target role
resources sharing heap
```

---

# 18. Resource classifier v0

The first classifier is rule-based.

Example evidence:

```text
Texture2D
+ SRV
+ mip_levels > 1
+ no UAV
+ no RTV/DSV
```

may become:

```text
probable visual sampled texture
confidence 0.8
```

But not automatically GREEN until enough evidence exists.

Another:

```text
Texture2D
+ UAV
+ repeated compute dispatch relation
```

becomes:

```text
probable data/compute texture
RED or UNKNOWN
```

The classifier must record **why** it chose a class.

Example:

```text
safety=GREEN_CANDIDATE
reasons=[
  SAMPLED_RESOURCE,
  HAS_MIPS,
  NO_UAV_HISTORY
]
```

This is essential for debugging wrong decisions later.

---

# 19. Resource temperature

Compute simple metrics:

```text
recency
frequency
reuse interval
burstiness
size
```

First temperature model:

```text
HOT:
used in recent small window

WARM:
not current, but historically likely to return

COLD:
not used for long relative to its normal reuse interval

PINNED:
unsafe/required

UNKNOWN:
insufficient evidence
```

Do not evict yet. Stage 1 only reports.

---

# 20. Trace storage

Suggested package per session:

```text
session/
├── session.json
├── hardware.json
├── events.arcbin
├── resources.sqlite
└── summary.json
```

`events.arcbin` is append-oriented binary.

`resources.sqlite` is produced either live at low frequency or post-processed from the trace.

For maximum hot-path safety, prefer post-processing initially.

---

# 21. Trace crash recovery

Write data in chunks.

Each chunk:

```text
magic
schema
chunk sequence
payload bytes
optional checksum
payload
```

If process crashes, the reader should recover all complete chunks and ignore the final partial chunk.

---

# 22. Trace viewer v0

Must answer visually:

## Timeline

```text
frame time
GPU/CPU utilization where available
VRAM budget
VRAM usage
allocation events
```

## Resource list

Sortable by:

- size;
- type;
- lifetime;
- last use;
- use count;
- safety;
- mip count.

## Resource detail

Show:

```text
identity
dimensions
format
estimated bytes
heap
views
queues
lifetime
usage timeline
classifier evidence
```

## Memory composition

At a selected moment:

```text
textures
buffers
RT/depth
unknown
```

---

# 23. Synthetic test: dx12-memory-pressure

Purpose:

Validate budget capture and resource accounting.

Behavior:

1. allocate baseline resources;
2. increase memory use in controlled increments;
3. cross 50/70/85/95% of reported budget;
4. optionally approach/temporarily exceed budget in a safe test environment;
5. measure frame-time behavior;
6. free resources in known order.

Ground truth file contains:

```text
expected allocation IDs
expected byte sizes
creation order
destruction order
```

ARC output is automatically compared.

---

# 24. Synthetic test: dx12-texture-pressure

Create textures across:

```text
512²
1024²
2048²
4096²
8192² where supported/practical
```

Formats include:

- uncompressed;
- BC formats;
- mipmapped/non-mipmapped.

Use deterministic camera/visibility sequence.

ARC must correctly record:

- dimensions;
- mip count;
- allocation;
- descriptor relationship;
- lifetime;
- use pattern.

---

# 25. Synthetic test: dx12-mixed-resources

Mix:

- sampled textures;
- UAV textures;
- render targets;
- depth;
- vertex/index buffers;
- indirect buffers;
- copy staging;
- transient resources.

Purpose:

Validate classifier evidence and prevent "all textures are visual" mistakes.

---

# 26. Observer correctness report

Every test run outputs:

```text
resource_create_recall
resource_destroy_recall
byte_accounting_error
descriptor_mapping_accuracy
queue_mapping_accuracy
event_overflow_count
trace_parse_errors
```

The build should fail CI if core synthetic correctness drops below threshold.

---

# 27. Observer overhead report

Measure:

```text
baseline
observer-light
observer-full
```

Report:

```text
average FPS
median frame time
P95
P99
CPU overhead
memory overhead
trace bandwidth
```

Observer-light will eventually be production mode.

Observer-full is diagnostic.

---

# 28. Stage-1 CI

CI should at minimum:

- compile Debug/Release;
- run unit tests;
- run headless/core tests;
- validate trace writer/reader;
- validate event schema versioning.

GPU integration tests may require a dedicated self-hosted machine.

---

# 29. Coding priorities

Optimize for:

1. correctness;
2. trace integrity;
3. low overhead;
4. backend neutrality;
5. ergonomics.

Do not prematurely optimize UI.

---

# 30. Explicit "do not do this yet" list

Do not yet:

- evict resources;
- alter mips;
- rewrite shaders;
- change resolution;
- enable frame generation;
- train RL;
- inject per-game hacks;
- write custom GPU KMD;
- claim compatibility with protected games.

---

# 31. Stage-1 completion gate

Stage 1 is done when a long controlled run can produce a trustworthy statement like:

```text
Peak OS budget:              5.4 GB
Peak observed game usage:    5.1 GB

Observed resources:          18,200

Texture allocation:          3.5 GB
Buffers:                     0.9 GB
RT/depth:                    0.5 GB
Other/unknown:               0.2 GB

Likely visual mipmapped:
                             2.6 GB

Cold for >X frames at peak:
                             0.8 GB

Classifier unsafe/unknown:
                             1.4 GB

Observer frame-time delta:
                             +0.15 ms P50
                             +0.22 ms P99
```

Numbers above are illustrative.

What matters is that the report is **measured**, internally consistent, and reproducible.

At that point Stage 2 may start making reversible residency decisions.
