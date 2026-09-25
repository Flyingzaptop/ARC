# Exact large sort: isolated GPU economic check

From `1b44758`. **Negative result for this implementation and operation boundary.**
The GPU result preserves the original complete byte permutation, including tied
keys, but the full GPU path is **15.48–17.62 times slower** than the original CPU
code. No game was started or attached, no full trace collected, and no ownership
mechanism or pattern-exchange system was added.

## Measured result

Saved real input: **65,344 records × 16 bytes = 1,045,504 bytes**. CPU reference is
the original four-function machine-code closure, not Python or a replacement
`std::sort`. Both implementations are checked against the saved original output.

Each series contains eight measured CPU/GPU pairs, with alternating order and one
separate warmup pair. These are operation trials, not frame/FPS measurements.
Final performance runs have the D3D12 debug layer disabled.

| Median, ms | Series 1 | Series 2 |
|---|---:|---:|
| Original CPU sort, result ready in CPU memory | **4.1342** | **3.4270** |
| Host preparation / upload-memory copy | 0.1110 | 0.1233 |
| CPU command recording | 0.2206 | 0.2039 |
| CPU submission | 0.1308 | 0.1042 |
| CPU wait for GPU completion | 63.3630 | 59.7726 |
| Copy returned data to CPU consumer buffer | 0.1427 | 0.1199 |
| **Full GPU route, CPU input ready → CPU output ready** | **64.0104** | **60.3693** |
| GPU upload interval | 0.0883 | 0.0910 |
| GPU task initialization | 0.0040 | 0.0036 |
| **GPU sorting / task scheduling interval** | **62.9612** | **59.2717** |
| GPU return interval | 0.0962 | 0.0857 |
| First/root GPU partition, part of sorting interval | **12.8865** | **13.1354** |
| Full GPU / CPU ratio | **15.48×** | **17.62×** |

Every measured GPU full-path sample is slower than every CPU sample in its series.
Column medians need not add up; the analysis script checks the wall-time component
sum for every individual GPU row. GPU intervals are nested in CPU wait/full time
and must not be added to them again.

Cold initialization (device, buffers, shader compilation and pipelines) was
836.98 / 853.28 ms in the two performance processes. It is reported separately,
not hidden inside a claim of per-call acceleration. Resources and mapped buffers
are reused between trials. File loading, input reset for CPU, and correctness
comparison after completion are outside the per-call timings. GPU upload preparation,
recording, submission, wait and final CPU-ready copy are **inside** its full route.

## Implemented GPU algorithm

`experiments/exact-sort-gpu/exact_sort.hlsl` ports the recovered exact-order model:

- original unsigned 64-bit key, represented as two uint32 words;
- median-of-three / ninther preparation and identical equal-key swaps/rotations;
- original insertion leaves and heap fallback;
- unchanged depth-budget reduction;
- parallel execution of already-disjoint child ranges using two bounded task buffers.

Each active partition is executed by one shader invocation; child partitions run
concurrently. Queue append order may differ, but children touch disjoint ranges,
so it cannot alter record order. No alternative tie-breaker or stable partition
is substituted. Two UAV task counts and GPU-generated indirect dispatch arguments
avoid per-level CPU readback. All 37 bounded rounds, including empty tail rounds,
barriers and indirect scheduling are included in measured GPU sorting time.

The standalone D3D12 harness uses its own DEFAULT/UPLOAD/READBACK resources and a
dedicated queue on its own device. Each call uploads the array, runs the sort,
returns the complete array, waits for a fence, and copies into ordinary CPU memory.
Exact output validation occurs after the readiness timestamp. This is **not**
automatic task extraction, live substitution or game acceleration.

## Correctness

26 GPU invocations / **1,453,952 record comparisons**, zero mismatches, task overflow
or unfinished tasks. This includes warmups and validation runs, separately labelled
from the 16 measured performance pairs.

The debug-layer validation covers:

1. Real 65,344-record input and its saved original output.
2. 65,344 records with two keys and different payloads: exact tie permutation.
3. 4,096 random unsigned keys, including high-bit values.
4. 4,096 records forced through heap fallback.

For generated cases, expected output is produced by the recovered model and then
independently checked by the original native code as well as GPU. The harness checks
D3D12 error/corruption messages, queue overflow, completion and all output records.
These checks establish this experiment's results, not arbitrary-input/live-program
equivalence or a production-ready GPU sorter.

## Why it loses

This is predominantly a **GPU computation/control-flow problem in the port**, not
transfer cost: upload and return GPU intervals together are about 0.18 ms, versus
59–63 ms sorting. The root starts with only one active partition and takes about
13 ms by itself—already more than the whole CPU sort.

Exact sequential swap order limits the initial parallelism of this implementation.
Later tasks expose parallel work, but retain sequential per-partition loops. No
hardware-counter evidence was collected to divide the remaining cost precisely
between divergence, instruction execution and memory latency. GPU utilization
percentage alone does not establish effective parallel utilization.

This result does **not** prove every exact-permutation GPU algorithm must lose.
It does show that this straightforward exact-order task-parallel port provides no
economic basis for building live-memory ownership machinery for standalone sort.

## Decision and next boundary

**Stop ownership development for this standalone sort.** There is no positive
budget into which instrumentation, certification and additional guards could fit.

Simply removing readback or keeping the same exact-sort output on GPU cannot fix
the observed problem: the root compute interval alone exceeds the CPU sort. A
wider chain would need measured savings elsewhere or a different proven parallel
algorithm; avoiding a ~0.1 ms return is not a solution to ~60 ms computation.

Two distinct alternatives were considered:

| Alternative | Assessment |
|---|---|
| Build queue → sort → GPU instance consumer | The earlier source-assisted resident chain was positive, but it used a different equal-key ordering under a restricted rendering contract. Its gain is not evidence for the present exact-order algorithm. Reopening this direction requires proving consumer invariance for a limited class or a faster exact algorithm, followed by a new isolated economic check. |
| Keep native CPU sorting; investigate the automatically found bitmask/instance packing consumer as one batch ending in its GPU buffer | Better next bounded candidate. Preserve original sorted record order; assess actual whole-batch CPU cost, offsets/counters and publication requirements first. Do not launch one GPU packet per 1–16-word inner loop or attribute the entire RenderMeshes parent cost to it. Pure-loop cost remains unknown. |

The discovered index-generation path remains a buffer-size-change task with unknown
recurring frequency; it is not promoted as per-frame savings. No second transfer
mechanism was built in this pass.

## Hardware, bounds and artifacts

NVIDIA RTX 3060 Laptop GPU, driver 616.64. Session telemetry has 70 complete samples
and one incomplete last row (preserved, excluded from aggregates): temperature
44–47 °C, graphics clock 1702–2002 MHz, power 21.76–33.34 W. Power-limit query is
unavailable. No power/cooling settings changed. These samples cover the whole
experiment, including compilation/validation, not individual kernels.

Explicit D3D12 buffer sizes total 4,704,860 bytes, including upload/readback and
two task buffers; this is not total process RAM or total driver/VRAM commitment.
The harness caps input at 4 MiB, repetitions at 30 and fence wait at 15 seconds;
the runner also caps each process at 45 seconds. No renderer contention was tested.

- [Independent analysis](../evidence/exact-sort-gpu-20260925/analysis.json)
- [Raw timings, generated cases, hardware log and runnable EXE/shader](../evidence/exact-sort-gpu-20260925/raw-and-package.zip)
- [Archive hashes and original-input reference](../evidence/exact-sort-gpu-20260925/archive-manifest.json)

Build and reproduce:

```powershell
cmake --build build --config Release --target arc-exact-sort-gpu -j 2
python experiments/exact-sort-gpu/run.py <cpu-contract-study-root> <new-output-directory>
python experiments/exact-sort-gpu/analyze.py <output-directory> <analysis.json>
```

The original real input/output and complete closure are reused from the prior
committed CPU-contract evidence archive. No new full instruction trace was made.
