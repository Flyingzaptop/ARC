# Automatic CPU task discovery: first gate, 2026-09-24

**Result: automatic GPU offload is NOT implemented.** The bounded observer and machine-code
screen found candidates without target names/addresses, but did not establish a complete,
safe CPU task with memory/dependency/consumer closure. The task's stop condition applies.
No GPU replacement, fictitious auto-transfer arm, scheduler or engine adapter was added.

## What changed

- Added `arc-cpu-task-probe`: external Windows thread CPU/cycle inventory plus selected
  register contexts. No injected DLL, whole-program DBI, function-name whitelist or PDB.
- Context capture is separate from cheap activity polling. At most four threads per
  100 ms tick, at most 12 seconds. A visible-window owner gets a sampling slot as a
  potential frame-thread hint, not proof of a critical path. Worker CPU totals are not
  counted as removable frame time.
- Suspend/GetThreadContext/Resume is guarded with RAII. A single roundtrip over 5 ms or
  aggregate roundtrip time over 50 ms stops further contexts after that sample. These
  are diagnostic backstops, not hard real-time guarantees. Counters continue to the
  fixed deadline. This tool is a bounded study, not an always-on production profiler.
- Captured live executable-section bytes and a PE copy pin the analyzed generation.
  PE exception-directory bounds identify regions without symbols. The analyzer rejects
  mismatching live/disk code. It does not infer access to all CPU computation from DX12.
- Added name-blind backedge screening, memory operand/direction extraction, prospective
  EAs, induction candidates, register recurrences, call/branch/atomic refusals, and a
  structural paired-record-exchange recognizer. Corrected Capstone 5.0.6's AVX store
  access metadata using decoded move direction; regression covered explicitly.
- Kept all safety unknowns explicit: invocation count and sizes, full input/output ranges,
  aliasing, concurrent writers, exit state, consumers, cost and critical-path gain are null.

## Native observation

The owned Wicked Instances host from `1c8a51e` ran original execution. No resident
adapter or generic GPU DLL was active. Object animation was phase-aligned to the
8-second warmup; 12 seconds of frame measurements include the 10-second discovery
probe and its preparation/analysis. Baseline and discovery use the same executable.

The final probe produced **376 contexts, 19 resolved main-image function regions and
12 overlapping backward-branch candidate spans**. These are not 12 independent tasks.
The snapshot collector used **48.603 ms total Suspend/GetContext/Resume roundtrip time**
across 10.007 seconds. This sum is not the complete observer overhead or a critical-path
cost. There were no failed contexts or final-run budget stops. Many sampled contexts
(282) landed in ntdll; these snapshots do not establish Running/Ready/Waiting attribution.
Interval CPU activity cannot be assigned wholesale to the sampled instruction.

## Automatically discovered candidate table

RVA values below are *outputs*, never detector configuration. Names/symbols and engine
source were not used. Overlapping candidates share samples; do not sum their counts.

| Loop RVA → backedge | Samples | Memory operands / stores | Structural hint | Decision |
|---|---:|---:|---|---|
| `1d78d0 → 1d7a27` | 15 | 33 / 4 | unclosed loop | Original; calls_have_unclosed_effects_and_dependencies, internal_control_flow_not_proven_elementwise |
| `223764 → 2238e3` | 14 | 30 / 8 | conditional_record_exchange | Original; internal_control_flow_not_proven_elementwise, memory_address_recurrence_not_proven_affine |
| `223764 → 223921` | 14 | 38 / 12 | conditional_record_exchange | Original; internal_control_flow_not_proven_elementwise, memory_address_recurrence_not_proven_affine |
| `223764 → 22393c` | 14 | 42 / 14 | conditional_record_exchange | Original; internal_control_flow_not_proven_elementwise, memory_address_recurrence_not_proven_affine |
| `2235a0 → 2239a4` | 14 | 59 / 15 | conditional_record_exchange | Original; calls_have_unclosed_effects_and_dependencies, internal_control_flow_not_proven_elementwise |
| `223810 → 22389e` | 8 | 10 / 2 | conditional_record_exchange | Original; internal_control_flow_not_proven_elementwise, memory_address_recurrence_not_proven_affine |
| `1c9e90 → 1ca188` | 6 | 87 / 40 | unclosed loop | Original; calls_have_unclosed_effects_and_dependencies, internal_control_flow_not_proven_elementwise |
| `223770 → 2237f7` | 5 | 10 / 2 | conditional_record_exchange | Original; internal_control_flow_not_proven_elementwise, entry_bounds_and_task_size_unknown |
| `1d1180 → 1d122d` | 3 | 21 / 7 | unclosed loop | Original; calls_have_unclosed_effects_and_dependencies, internal_control_flow_not_proven_elementwise |
| `230f13 → 231a9b` | 2 | 254 / 39 | unclosed loop | Original; simd_loop_carried_state_requires_recurrence_semantics, calls_have_unclosed_effects_and_dependencies |
| `230f07 → 231ac3` | 2 | 258 / 41 | unclosed loop | Original; simd_loop_carried_state_requires_recurrence_semantics, calls_have_unclosed_effects_and_dependencies |
| `2448f0 → 244a0b` | 2 | 36 / 15 | unclosed loop | Original; calls_have_unclosed_effects_and_dependencies, internal_control_flow_not_proven_elementwise |

## Concrete candidate and missing mechanism

The selected candidate `a0a0babad8bef48b:223810-22389e` was selected from the detector output. Its
machine code reads packed fields at offsets 0, 8 and 12, constructs comparison values,
and conditionally exchanges two **16-byte records** using vector loads/stores. The code
includes conditional flow and pointer advancement. A record-permutation/partition
primitive is therefore a more relevant follow-up than inventing a convenient synthetic
array map. The structural exchange is recognized; **a whole-sort semantic contract is
not proven**. This result did not come from handing the detector the known Wicked sort.

The current sample proves neither the invocation's initial array bounds nor its complete
record set. Conditional advancement means a static `+16` is not proof of a uniform,
independent iteration. Sources/destinations can overlap; writes and ordering can affect
later iterations. Return state and the first consumer are unknown. Copies of registers
at occasional interior PCs cannot recover those facts. Consequently the independent-map
screen refuses this region and a GPU sort cannot safely be substituted either.

The exact missing channel is **bounded, selected-region entry/exit and memory/dataflow
capture**, including callable effects and thread/job ownership, sufficient to derive
complete invocation ranges, prove dependencies and locate the first consumer. It must
also establish a useful CPU critical-path cost rather than sum worker times. Existing
M0–M4 instrumentation handles small register MOV/LEA regions; extending that directly
to these memory loops is not already implemented. Continuous whole-program DBI was
not enabled to hide this gap behind a severe slowdown.

## Timing comparison

One original/discovery pair establishes that this bounded channel did not reproduce the
previous multi-fold slowdown on this fixture. It does not resolve sub-millisecond overhead
or establish an optimization win. There were **zero replaced invocations and zero GPU
transfer bytes**. The auto-offload arm was not run because no task passed admission.
The second-renderer portability/performance gate is likewise not reached.

| Mode | Metric | Mean ms | Median ms | p95 ms | p99 ms |
|---|---|---:|---:|---:|---:|
| original | CPU through submission | 21.522 | 21.058 | 25.509 | 28.207 |
| original | GPU span | 6.340 | 6.311 | 6.693 | 7.753 |
| original | Present interval | 21.532 | 21.065 | 25.529 | 28.252 |
| discovery | CPU through submission | 21.398 | 20.993 | 24.731 | 26.569 |
| discovery | GPU span | 6.284 | 6.288 | 6.596 | 7.007 |
| discovery | Present interval | 21.408 | 21.008 | 24.790 | 26.611 |

Present cadence: **46.44 Hz original / 46.71 Hz discovery**. This tiny difference is not
attributed to the observer and is not an offload gain. Present cadence is not displayed
FPS. GPU ranges are joined to source device frames; unretired tail samples remain absent.

## Validation and limits

Release native build passed; both final renderer runs exited normally. Nine focused
decoder tests passed: independent-map shape still needs ownership proof, calls, atomics,
pointer chasing, SIMD recurrence, shared output addresses, missing registers, correct
AVX store direction, and generic record exchange. These test the observer/screen, not a
GPU replacement or universal correctness proof.

An earlier 2 ms per-sample cutoff stopped after 18 contexts; another intermediate capture
stopped at the aggregate budget. Those diagnostic attempts are retained separately and
not used for the timing comparison. One analyzer error on an absent address register
was fixed and covered by a test. The final capture is immutable and can be reanalyzed
without rerunning the game. No commercial games, new source-assisted adapter or historical
full performance matrix was used.

## Delivery

- [Candidate table](../evidence/cpu-task-discovery-20260924/candidate-table.csv).
- [Exact selected machine-code candidate and contexts](../evidence/cpu-task-discovery-20260924/selected-candidate.json).
- [Raw data](../evidence/cpu-task-discovery-20260924/raw-measurements.zip), including captured
  PE/code, all native samples, thread activity, frame CSVs and failed diagnostic attempts.
- [Reproduction instructions](../../benchmarks/cpu-task-discovery/README.md).

```powershell
C:/Python314/python.exe scripts/run-cpu-task-discovery.py C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-discovery-repeat
```

Admission remains blocked. Automatically discovering a safe and profitable transferable
task, generating/replacing its implementation, and expanding offload are **unfinished**.
