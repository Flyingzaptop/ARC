# ARC binary trace schema 4

The currently supported binary ABI is little-endian Windows x64/MSVC. Enum values
are fixed by `include/arc/events.hpp`; typed payload layouts are defined in
`include/arc/resource_graph.hpp`. No backend pointers are serialized. Changes to
payload layout require a new major schema. Pre-schema-4 development traces are
not compatible and are rejected explicitly.

Each chunk is a 24-byte header followed by at most 16 MiB of payload:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | uint64 | magic 0x314B4E4843524141 |
| 8 | uint32 | schema 4 |
| 12 | uint32 | consecutive chunk sequence, starts at zero |
| 16 | uint32 | chunk payload bytes |
| 20 | uint32 | FNV-1a checksum of payload |

The payload concatenates event headers and their actual payload bytes. It does
not serialize unused portions of the in-memory 256-byte envelope. Event headers
are 32 bytes: timestamp_ns (uint64), sequence (uint64), thread_id (uint32), type
(uint16), flags (uint16), payload_bytes (uint32), reserved (uint32).

Unknown event types can be skipped by payload length and are retained by the
reader. Known graph payloads with incorrect sizes produce graph errors. Reader
statuses distinguish Complete, TruncatedTail, CorruptTail, SchemaMismatch and
IoError. Only complete checksum-valid chunks are committed to recovered output.
An incomplete final header/payload leaves previous valid chunks readable.

The session sidecar `<trace>.session.json` reports completeness and overflow.
TraceOverflow also exists in the event stream so losing the sidecar cannot make
an overflowed capture appear correct to the graph/viewer.

Event flag bit 0 indicates a globally sequenced multi-producer stream. The
collector may write ready chunks out of sequence; the reader sorts these events
by the shared global sequence before graph reconstruction. Mixed global/local
ordering flags in the same trace are rejected. Producer ID and CPU timestamps
remain available independently of serialization order.

## Time and ownership

CPU observation timestamp and per-stream sequence are retained independently of
Present markers. Recording a command list does not count as executing it. Reset
clears pending command contents; each submission applies the recorded copy/read/
write relationships to its queue. Completed history remains in the raw trace.

`allocation_bytes` is the API allocation footprint of a resource, not exact
physical residency. A placed resource references its heap and offset. Heap bytes
are counted once, regardless of placed-resource overlap. A reserved resource's
zero allocation means no physical attribution is available, not zero virtual size.

GREEN_CANDIDATE is analysis only. View evidence flags use bit `1 << ViewType`;
UAV evidence is retained across descriptor overwrites. Unknown metadata never
authorizes modifying a resource.
