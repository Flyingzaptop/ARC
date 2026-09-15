# Trace format draft

## Goals

- compact;
- append-friendly;
- recoverable;
- versioned;
- cheap to write;
- decodable offline.

## Session metadata

`session.json`:

```json
{
  "schema": 1,
  "arc_version": "0.1",
  "api": "D3D12",
  "game": "sample",
  "hardware_profile": "calibration-id"
}
```

## Binary event header

```cpp
struct EventHeader {
    uint64_t timestamp_ns;
    uint64_t sequence;
    uint32_t thread_id;
    uint16_t type;
    uint16_t flags;
    uint32_t payload_bytes;
};
```

## Event classes

- ResourceCreate
- ResourceDestroy
- HeapCreate
- HeapDestroy
- DescriptorCreate
- DescriptorUpdate
- Barrier
- Copy
- Draw
- Dispatch
- QueueSubmit
- FenceSignal
- FenceWait
- Present
- MemoryBudget
- GovernorDecision
- GovernorRollback

## Versioning

Readers must reject incompatible major schema versions and tolerate unknown optional event types where possible.
