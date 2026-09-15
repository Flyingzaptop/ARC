# Stage 1 implementation status

Current gate (2026-09-15): practical Stage 1 validation has passed for the explicit
integration and supported controlled workloads. Graphics Tools is installed;
Debug/Release pass 6/6 tests with the D3D12 debug layer, and the corrected sample
passes 100,000 iterations with debug validation. See [the gate update](STAGE1_GATE_UPDATE.md).

This implementation is an explicit D3D12 integration surface, exercised by
controlled workloads. It is not an automatic interceptor for arbitrary games.
No Stage 2 policy or resource mutation is implemented in the observer.

## Safety contract

- The integration calls observation hooks after successful native API calls.
- Resource COM pointers are borrowed only during creation observation; the core
  never stores backend pointers. Owners supply stable IDs for later events.
- One observer/ring has one producer thread. Multiple producers must use distinct
  streams and a shared ID allocator. `MultiSession` provisions per-producer rings
  before capture and merges using a shared atomic event sequence. Each Observer
  receives its ring and the session's global sequence. Concurrent calls into one
  ring remain unsupported. Producers must join before finishing a session.
- Hot event emission performs fixed-size copies, monotonic timestamps and
  atomics. There is no per-event allocation, string formatting or file access.
- Resource creation observation makes read-only description/allocation/format
  queries. GPU timestamp commands, synchronization and test shaders belong to
  the synthetic workload and are identical in baseline and observed runs.
- Capturing does not alter contents, descriptors, shaders, barriers or command
  ordering. The collector persists events; post-processing builds the graph.
- Overflow is counted, persisted as TraceOverflow and marks the session incomplete.

## Implemented surface

| Component | Implementation |
|---|---|
| Resources/heaps | Committed, placed, reserved, external swapchain resources; stable IDs and destruction; placed heap offset; logical vs API footprints |
| Memory accounting | Resource footprint sum, committed allocation sum and heap allocation sum are distinct; placed footprints may overlap and must not be added to their heap again |
| Texture model | Dimensions, mips, array layers, sample count, queried planes; logical BC1/2/3/4/5/7 and RGBA8/D32/R32 estimates; per-mip utility with copy-row alignment |
| Views | SRV, UAV, RTV, DSV normalization for supported dimensions; CBV ranges; sampler identity; historical view evidence survives overwrites |
| Commands | Create/reset/close, copy/resource/texture/resolve records; submission replays recorded uses, including command list reuse |
| Work counters | Draw, indexed draw, dispatch and indirect counters per submitted list; per-call records in Full mode |
| Timelines | Independent graphics/copy submissions, GPU and CPU fence waits/signals, barriers, swapchain Present and result codes |
| Budgets | LOCAL/NON_LOCAL budget, usage, reservation and availability; start, periodic and pressure-step samples |
| Graph | Lifetime-at-time, heap membership, largest resources/textures, view history, queue membership, unused presentations and copy relationships |
| Analysis | UNKNOWN, GREEN_CANDIDATE, YELLOW, RED; confidence/view evidence; HOT/WARM/COLD/PINNED/UNKNOWN using recency and observed reuse intervals |
| Trace | Schema 4, compact records, chunk sequence/length/checksum, bounded chunk reader, complete-chunk recovery and explicit failure statuses |
| Calibration | CPU single/multi, RAM copy/multi-copy/pointer chase, non-destructive cached file reads; warmup and nine measured iterations; GPU draw/compute/upload/readback timestamps |
| Inspection | JSON session/summary/metrics; timeline CSV and full resource-table CSV |
| Validation | Concurrent ID uniqueness, SPSC wrap/order/overflow, trace recovery/corruption/schema, graph/view/lifetime/temperature tests; three GPU workloads |

## Known limitations and completion qualification

- Windows Graphics Tools was installed on 2026-09-15 through user-approved UAC;
  no restart was required. Debug-layer validation uncovered an unsafe sample
  teardown after Present. A final graphics fence now retires presentation work
  before resources are released. `-DebugLayer` enables this regression check.
- Enhanced barriers are capability-probed and normalized into pointer-free
  layout/access/sync/range records. Legacy aliasing/UAV records are supported too.
  Normalization is unit-tested. The GPU tests exercise legacy transitions and
  enhanced texture transitions with readback verification. Enhanced buffer/global
  and aliasing execution coverage is not exhaustive.
- No automatic COM wrapping, descriptor handle registry, per-game injection
  or arbitrary engine pipeline inference exists.
  Explicit hooks cover controlled workloads; they cannot claim whole-game recall.
- Several uncommon view dimensions return UNKNOWN; typed/raw buffer SRV/UAV byte
  lengths are unknown when no structure stride is supplied. Sampler parameters
  and UAV counter resources are not modeled. Descriptor heap lifetime, locations
  and scalar descriptor copies are tracked; range APIs must be flattened by the
  integration into scalar copies in the same order.
- Unsupported formats return logical estimate zero (unknown). Reserved resources
  have zero allocated physical bytes until mappings are known; tile mappings are
  not observed. External swapchain physical allocations are not attributed.
- Cold classification requires observed use history. Resources never observed
  in a submission stay UNKNOWN rather than being assumed safely evictable.
- Queue timestamps are CPU observation times. GPU timestamps measure only the
  controlled proxies; they do not turn all API-call timestamps into GPU execution
  timestamps. Present is a presentation interval marker, not proof of display scanout.
- Storage measurements are cache-warmed reads of a selected existing file, not
  raw device throughput. CPU multi tests include thread startup. XPRESS-Huffman
  decompression is measured on deterministic synthetic data. Random storage
  cold-cache latency, queue-overlap calibration and copy-queue GPU
  timestamp calibration are not yet implemented.
- Light and Full modes apply to the integration workload's event policy; a
  general renderer would need to aggregate its own command-list counters.
- There is no GUI or SQLite database; reconstructing a large trace currently
  retains events and graph history in memory during offline analysis.
- Core/sample outputs are valid for the tested machine and workloads. The practical
  Stage 1 gate is passed for that scope; this is not unrestricted native-game
  compatibility certification. CPU overhead <2% remains an unproven engineering
  target. Stage 2 requires a separate user task and conservative capability gates.

## Technical references used

- [D3D12 allocation info](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getresourceallocationinfo%28uint_uint_constd3d12_resource_desc%29)
- [Copyable footprints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints)
- [Enhanced barrier capability](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options12)
