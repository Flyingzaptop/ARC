# Stage 1.1 hardening report

Date: 2026-09-15. Tested on Windows build 26200, MSVC 19.50, an
i7-11800H and an RTX 3060 Laptop GPU.

## Ordering

MultiSession now performs a bounded online k-way merge. It holds at most one
head event per producer and writes only the next expected global sequence. It
does not sort a completed trace in memory. A missing sequence stalls the
collector while producers continue without blocking; ring overflow marks the
session incomplete instead of emitting a plausible reordered trace.

The adversarial test runs three rounds of 2,000,000 events. Producer zero delays
the first reserved sequence and remains bursty, producer one runs continuously,
producer two alternates yields and producer three emits bursts. All 6,000,000
events were written in strict order with zero drops. A second four-thread causal
test orders resource create, descriptor write, resource use/submission and
destroy across different rings. Reconstruction reported zero graph errors and
matched the expected lifetime/read relationship.

## TraceWriter changes

- The packing vector is retained and reused instead of allocated per append.
- Collector batches increased from 256 to 4096 events; the batch is allocated
  once on the heap. An attempted stack allocation was caught as Windows stack
  overflow during testing and corrected before commit.
- The writer flushes a checkpoint every 64 chunks and at clean shutdown instead
  of flushing every chunk.
- The current policy can lose the current uncheckpointed buffered tail after a
  process/OS crash. At most 63 completed in-process chunks can be pending before
  the next explicit checkpoint. Readers recover only complete checksum-valid
  chunks; ordinary clean shutdown checkpoints all data.
- TraceWriter and Session publish CPU phase and buffer statistics.

## Performance

Both matrices used three rotated rounds of 10,000 iterations per mode. The
September 14 matrix is the before reference. The final matrix includes reusable
packing, checkpoint batching, offline graph timing and a 160-byte event envelope.
The runs occurred in different system conditions, so cross-matrix absolute
changes are descriptive rather than a controlled single-variable experiment.

| Mean metric | Before baseline | Before Light | Before Full | After baseline | After Light | After Full |
|---|---:|---:|---:|---:|---:|---:|
| P50 ms | 0.50033 | 0.50723 | 0.50637 | 0.44693 | 0.44780 | 0.44733 |
| P95 ms | 0.87733 | 0.89597 | 0.87420 | 0.77323 | 0.79080 | 0.77647 |
| P99 ms | 4.54237 | 4.55670 | 4.54453 | 1.09657 | 1.08123 | 1.07703 |
| Total process CPU ms | 2744.79 | 2916.67 | 3062.50 | 2197.91 | 2140.62 | 2031.25 |
| Private memory MiB | 266.71 | 283.51 | 283.78 | 268.20 | 279.10 | 278.44 |
| Trace MB/s | 0 | 2.95 | 3.52 | 0 | 4.15 | 4.83 |

Within the final matrix, Light minus baseline is +0.00087 ms P50, +0.01757 ms
P95 and -0.01533 ms P99. Full minus baseline is +0.00040 ms P50, +0.00323 ms
P95 and -0.01953 ms P99. Negative deltas are measurement noise, not performance
claims.

Final phase averages per 10,000 iterations:

| Phase CPU ms | Baseline | Light | Full |
|---|---:|---:|---:|
| Producer/main thread | 2156.25 | 2046.88 | 1916.67 |
| Collector | 0 | 26.04 | 36.46 |
| TraceWriter subset | 0 | below timer resolution in these rounds | 5.21 |
| Offline ResourceGraph | 0 | 93.75 | 104.17 |

The producer and total process measurements vary enough that a <2% producer CPU
claim is not supported. Background work and trace throughput are now isolated
and reported rather than inferred.

## Coverage hardening

Reserved resources now receive the D3D12 device and query resolved resource
description/plane metadata. UAV 1D-array/3D, RTV 1D/array/3D/2DMS-array and DSV
1D/array/2DMS-array views are normalized. Unknown stays the fallback.
ResourceGraph rejects missing/duplicate command and resource lifecycle events,
unknown resource references and illegal resource-bearing sampler records.

## Acceptance gate A

Release and clean Debug builds passed 9/9 tests with the D3D12 debug layer:
core, 6-million-event ordering stress, view normalization, enhanced barriers,
three observer GPU workloads, and baseline/ARC residency lab. The final observer
path also passed a 100,000-iteration Full run with the debug layer: data and
images matched, zero trace drops and graph errors, P50 0.8137 ms and P99
1.7155 ms. Stage 2A work began only after these results.

