# Stage 1 implementation and validation report

Date: 2026-09-14. Environment: Windows build 26200, MSVC 19.50,
Core i7-11800H, RTX 3060 Laptop GPU.

## Implemented

- Backend-neutral IDs, resource/heap/view/command models and temporal queries.
- Bounded SPSC transport, asynchronous capture, overflow diagnostics and offline
  graph reconstruction. MultiSession merges independently provisioned producers.
- Committed, placed, reserved and external resource observation; heap offsets;
  distinct logical estimates, resource allocation footprints and allocated heaps.
- Native view normalization, descriptor locations/heaps/copies and conservative
  historical classification. Legacy/enhanced barrier normalization.
- Graphics/copy queue submissions, fences, Present, recorded copy/resolve
  relationships, draw/indexed/indirect/dispatch counters and budget snapshots.
- Three GPU workloads with deterministic buffer, raster, MSAA and compute checks.
- Versioned CPU/RAM/decompression/storage profile and GPU timestamp proxies.
- CLI JSON summaries, timeline/resource CSV, Debug/Release tests and CI configuration.

## Commits

All changes are local; no remote repository was created or pushed.

- 65db2e7 Fix MSVC dependency tracking and run checks in Release
- 2b33660 Validate trace chunks and expose recovery diagnostics
- 85da7f4 Track submitted resource use and conservative classification
- 2a47b3f Collect bounded event streams asynchronously and drain on shutdown
- 191f6eb Validate D3D12 copy contents and resource lifetimes in three workloads
- 8bfae1a Estimate BC and mip payloads separately from API allocations
- 01634dd Measure CPU RAM and read-only storage calibration distributions
- fe38e46 Inspect traces offline with resource and session summaries
- 84ebc4a Validate heap ownership and reserved resources against an unobserved baseline
- 6eda763 Verify raster output with deterministic GPU image readback
- 19be38f Observe DXGI presents and synchronized graphics-copy queues
- 2a10358 Report measured process overhead and ground-truth recall
- a59ff49 Exercise indexed indirect and compute work with GPU data verification
- 527b043 Add optional D3D12 debug-layer error validation
- 578e5e8 Normalize descriptor views in the D3D12 adapter and preserve ranges
- 079b337 Test concurrent IDs and event ordering with temporal graph queries
- 2215609 Mark incomplete sessions and persist collector diagnostics
- 638a000 Measure GPU draw compute upload and readback proxies with timestamps
- 345be16 Validate native CBV sampler and SRV descriptor relationships
- b47592a Automate hardware profiling and rotated baseline observer benchmarks
- df43ebe Compact trace records and move graph analysis outside capture interval
- 1faf0a8 Add Debug Release CI and opt-in GPU integration tests
- 6a6e78a Preserve full resource widths and reject malformed graph events
- bb31357 Exercise configurable allocation pressure against the live DXGI budget
- 10cfe30 Validate MSAA resolves and resolved image contents
- 0e9ca3f Use observed reuse intervals in conservative temperature analysis
- 653a80f Record texture sample and plane counts with explicit schema version
- b02d7f2 Export budget queue and resource timelines for offline inspection
- c404907 Merge preallocated producer streams with global event ordering
- 39e4be6 Normalize legacy and enhanced barrier records without backend pointers
- 73765ef Track descriptor heap lifetime and native descriptor copies
- 10508ef Calibrate XPRESS Huffman decompression with verified output

## Validation

- Debug: 5/5 CTest tests passed, including three GPU workloads.
- Release: core/view tests and all three GPU workloads passed.
- Four producer threads: 32,000 globally ordered events; no missing resources.
- Concurrent IDs: 40,000 unique IDs. SPSC: 100,000 ordered events through a small
  wrapping ring. Additional session test: 20,000 create/destroy events.
- Trace tests: complete input, partial tail, corrupt tail, unknown event and schema
  mismatch. Known malformed graph payloads are rejected.
- Pressure run: 96 created/destroyed resources, 2,721,513,472 peak committed bytes,
  4,194,304 heap bytes; peak DXGI usage 2,736,467,968 of 5,479,858,176 budget.
- Rotated performance matrix: 3 rounds × 3 modes × 10,000 iterations. All nine
  runs passed; zero drops, zero occluded Present results.
- Continuous Full stress: 100,000 iterations; 3,901,779 events; 200,000 submissions;
  700,000 copies/resolves; 100,000 presents; 33 created and 33 destroyed resources;
  zero graph errors/drops/live resources. Buffer, raster, MSAA and compute outputs
  matched expected data. Create/destroy recall and checked descriptor mapping
  accuracy were 1.0; allocation error was zero.
- Stress capture took 46.452 s and wrote 242,220,400 bytes. P50 iteration time was
  0.4459 ms; P99 0.6345 ms. This standalone run is not an A/B comparison.

Raw measured JSON is checked in under [validation](validation/).
The rotated matrix predates the last optional multi-stream/descriptor additions;
the continuous stress run uses capture code at 73765ef. Calibration was generated
while implementing 10508ef; its recorded HEAD reflects the preceding commit.
These observations are not claims about arbitrary games or untested hardware.

## Observer overhead

The table contains the arithmetic mean of each run's P50/P99, not pooled quantiles.

| Metric | Baseline | Light | Full |
|---|---:|---:|---:|
| Mean of run P50, ms | 0.500333 | 0.507233 | 0.506367 |
| Mean of run P99, ms | 4.542367 | 4.556700 | 4.544533 |
| Summed process CPU time, ms | 8234.37 | 8750.01 | 9187.50 |
| CPU change vs baseline | — | +6.26% | +11.58% |
| Mean process private memory, MiB | 266.71 | 283.51 | 283.78 |
| Trace throughput, decimal MB/s | 0 | 2.95 | 3.52 |

Light mean P50 delta is +0.0069 ms; average extra private memory is about 16.8 MiB.
The <0.2 ms steady-state timing goal is met in this test. The <2% relative process
CPU target is **not demonstrated** by the aggregate data. CPU results vary by
round; do not replace the measured aggregate with a claim of zero overhead.
Private-memory measurements are during capture; offline parsing has a separate
larger memory cost. These iteration rates are not game FPS or displayed FPS.

## Stage 1 completion matrix

| Major component | Status | Scope |
|---|---|---|
| Core IDs/time/events and multi-producer transport | DONE | Tested bounded streams and global order |
| Resource/heap lifetimes and accounting | DONE | Explicit integration, controlled native allocations |
| Descriptor/view relationships | DONE | Common native dimensions, heap lifetime and scalar copies |
| Texture subresource model | PARTIAL | Supported logical formats; uncommon/planar formats explicitly unknown |
| Queues/commands/copies/fences/presents | DONE | Graphics and copy queues; submitted replay model |
| Barriers | PARTIAL | Legacy GPU execution; enhanced/aliasing normalization tests only |
| Memory budget telemetry | DONE | Local/nonlocal snapshots; polling and pressure steps |
| Hardware profile/calibration | DONE | Measured proxies, documented cache/overlap limitations |
| ResourceGraph/classifier/temperature | DONE | Conservative read-only analysis and temporal queries |
| Recoverable trace/session diagnostics | DONE | Schema 4; explicit incomplete/corrupt statuses |
| Ground truth and CLI inspection | DONE | Controlled workload coverage; JSON and CSV |
| Stability and output invariance | DONE | Tested synthetic scope, including continuous stress |
| Overhead measurements | DONE | Measured and disclosed; CPU target not yet established |
| D3D12 debug-layer validation | BLOCKED | Windows Graphics Tools unavailable |
| Full Stage 1 sign-off | BLOCKED | Do not equate synthetic coverage with unrestricted production readiness |

## Known limitations

See [the maintained scope and limitations](STAGE1_STATUS.md). The integration
surface deliberately does not promise automatic observation of arbitrary native
games. In particular, uncommon descriptor dimensions/typed buffer details,
sampler parameters, tile mappings, full engine/shader use inference and enhanced
GPU execution coverage are not certified. The GUI is replaced by CLI/CSV.
Cold-cache storage and measured queue overlap remain unavailable in the profile
rather than being assigned fabricated values.

## Blockers

D3D12 debug-layer validation returns DXGI_ERROR_SDK_COMPONENT_MISSING.
C:/Windows/System32/d3d12SDKLayers.dll is absent. Querying/installing the Windows
Graphics Tools capability requires elevation, unavailable in the current process.
This blocks the debug validation path, not the already completed GPU tests.

Technical options:
1. Install Windows Graphics Tools with an administrator account, then run
   ARC_D3D12_DEBUG=1 GPU validation here.
2. Run the same Debug/Release and debug-layer suite on a Windows machine with
   Graphics Tools installed (preferably a dedicated GPU CI runner).
3. Keep this as a provisional Stage 1 integration build while those checks and
   remaining coverage gaps are reviewed; do not enable Stage 2 policies.

## Recommended next action

Complete the missing debug-layer validation and review coverage/CPU overhead
before signing off Stage 1. Stage 2 has not been implemented and must be a separate
authorized task after that gate is accepted.

