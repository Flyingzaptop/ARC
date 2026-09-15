# 03 — Resource model

## 1. Stable resource identity

Never use raw API pointer values as permanent identity.

```cpp
using ResourceId = uint64_t;
```

Every creation gets a monotonically unique ID.

## 2. Resource record

Recommended initial shape:

```cpp
struct ResourceRecord {
    ResourceId id;

    ResourceKind kind;
    ResourceFormat format;
    ResourceUsage usage;

    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t array_layers;

    uint64_t virtual_bytes;
    uint64_t estimated_physical_bytes;

    uint64_t create_frame;
    uint64_t destroy_frame;

    uint64_t last_read_frame;
    uint64_t last_write_frame;

    uint64_t read_count;
    uint64_t write_count;

    QueueMask queues;

    SafetyClass safety_class;
    float safety_confidence;

    ResidencyState residency;
};
```

## 3. Texture subresources

Track:

```text
mip
array layer
plane
depth slice where applicable
```

Texture memory is not one indivisible block from ARC's perspective.

Future decisions may target only high mips.

## 4. Descriptor graph

ARC must map:

```text
descriptor
→ view
→ resource
→ subresource range
```

This is necessary to infer actual shader-visible use.

Track at least:

- SRV;
- UAV;
- RTV;
- DSV;
- CBV where relevant;
- sampler metadata.

## 5. Safety classes

### GREEN

Good candidate for automatic optimization after sufficient evidence.

Typical example:

- ordinary sampled mipmapped texture;
- never used as UAV;
- no suspicious integer-data access known.

### YELLOW

Potentially optimizable, but context-dependent.

Examples:

- shadow map;
- environment map;
- intermediate render target;
- reflection resource.

### RED

Do not modify automatically.

Examples:

- acceleration structures;
- indirect argument buffers;
- compute state/data;
- synchronization-related buffers;
- opaque engine data.

### UNKNOWN

Default state.

Unknown behaves as RED until evidence upgrades it.

## 6. Temporal resource graph

A static graph is insufficient.

ARC needs:

```text
resource A used in frames 100-140
unused 141-220
used again 221
```

From this derive:

- reuse interval;
- hotness;
- burstiness;
- promotion usefulness;
- prefetch confidence.

## 7. Virtual vs physical view

Especially for emulator integration:

```text
Guest Resource
     ↓
ARC Virtual Resource
     ↓
Host Physical Allocation(s)
```

Guest logical dimensions do not have to equal the set of physically resident host pages/mips, provided the translation preserves guest-visible behavior.

This is the foundation of low-VRAM operation.
