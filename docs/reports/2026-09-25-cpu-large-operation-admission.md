# Large CPU operation: independent semantics and memory gates

Continues `037d00b`. No new full trace, no game run, no pattern-exchange system,
no development of the 2–32-record case. The existing tracer is unchanged.

## Result

The large sorting operation now has an executable deterministic model which
preserves the original equal-key permutation. It matches the original machine
code on **49 cases / 1,074,241 records**, including the actual **65,344-record**
capture and forced heap fallback. This is substantially stronger evidence than
matching sorted keys or one rendered image, but is **differential validation,
not an all-input machine-equivalence certificate**.

The selected array's TLS/vector lifecycle was reconstructed from saved machine
code. Actual allocation birth/generation, all escaping aliases, and exclusion
through commit remain unproved. The executable admission checker evaluates
semantics and memory separately; **both gates currently remain closed**. No GPU
replacement or net speedup is claimed.

The precise limitation is that an entry/return tracer plus allocation logging
cannot certify an exclusive future interval. A trustworthy ownership provider
must prove or enforce the complete access domain, not just report no conflict.
Adding another observational logger without that mechanism would not change
admission, so no new observer was implemented in this pass.

## 1. Large computation: preserve exact order

Implemented `scripts/cpu_large_sort.py`: the observed unsigned-key projection,
median-of-three/ninther pivot preparation, original equal-range movement,
insertion leaves, depth-budget reduction and heap fallback. Whole 16-byte records
move together. No stable sort, ID tie-breaker, or relaxed consumer criterion is
substituted. Local compiler-library source was a cross-check of the interpretation,
not an input to detector selection or an engine-specific hook.

The isolated native oracle loads the four saved functions at their relative
offsets in its own process. It never attaches to the renderer. Code hashes are
pinned; full grouped unwind ranges are required. The move helper's reachable
insertion-sort operation is an overlapping right shift by one 16-byte record.
The native test does not install a loader for arbitrary game functions.

The first oracle attempt exposed incomplete **archived closure coverage**: the
old training bundle had only 18 bytes of the heap helper, while the existing
`logical-plan` had all 519. The isolated fixture faulted on the first forced-heap
case using that truncated bundle. Using the already saved complete closure fixed
the fixture. This was not a game crash, and did not require recapture.

### Validation

| Case family | What it checks |
|---|---|
| Real captured 65,344 records | Exact match to both saved original output and fresh isolated native execution |
| Random unsigned 64-bit keys | Integer key ordering, high-bit behavior and movement |
| All-equal / two-key inputs with distinct payloads | Exact original tie permutation, not canonicalized membership |
| Ascending / descending / organ-pipe | Pivot selection, rotations and boundary behavior |
| Counts 33, 41, 42, 257, 4,096, 65,344, 65,536 | Partition threshold and ninther boundary, large workloads |
| Forced budget zero | Heap fallback, including tied keys |

All 49 cases have zero record differences; canaries passed. On the real input,
the model performs 2,198 partitions and 354,551 swaps. Native isolated execution
took 4.0899 ms in that one run; **this is not a new application benchmark or saved
frame time**. Python model timings are not optimizer performance measurements.

Remaining semantic obligations are explicit: close the binary-to-model relation
for every admitted input/path, and preserve continuation state, faults and external
effects. Ordinary tests, even exact large tests, do not set those proof fields true.
Twelve data-comparison sites across the partition/insertion, median and heap bodies
need consistent key semantics; the prior four-site mask proof alone is insufficient.

### Concrete computational route to a GPU implementation

Keep the **same partition and tie movement**. Once a partition completes, its two
child subranges are disjoint and can be processed concurrently, retaining the same
reduced depth budget. Scheduling independent children in a different order does not
change their bytes. Each child's insertion/heap behavior must remain identical.
An arbitrary parallel/stable partition is not interchangeable: it changes the
starting order supplied to descendants and can change final tie order.

This route has a real risk: the initial exact partition remains serial, and staging
plus return to the CPU consumer may consume the gain. It must first be measured on
owned buffers after the transformation is validated, then under a valid application
lease. No GPU speedup is inferred from this CPU oracle.

## 2. Array lifecycle recovered from the existing image

[lifecycle.json](../evidence/cpu-large-admission-20260925/lifecycle.json) contains
the exact bytes, hashes and separate status of each edge. A readable disassembly
is saved alongside it. The image identity is pinned to the original detector image.

| Phase | Machine evidence | What is established / missing |
|---|---|---|
| Descriptor origin | GS TLS lookup, slot zero + `0x1b8` | Selected caller uses a thread-local begin/end/capacity descriptor; does not establish no escape |
| Allocation/growth | append `0x1c1040`, allocation call `0x1c10f1` → `0xaeec0` | New storage obtained before reallocation; actual birth event/generation absent |
| Raw vs data pointer | `0xaeef9..0xaef01` | Large blocks have a 32-byte-aligned interior data pointer; raw base saved at data−8. Logging only the data address misses allocator identity |
| Fill | `0x1d7a16`, `0x1c1150..0x1c119b` | Fields written, end advanced, full record stride 16 bytes |
| Sort | `0x1d7b74..0x1d7b87` | RCX=begin, RDX=end, R8=(end−begin)/16; captured count 65,344 |
| Consume | `0x1d7ebb..0x1d7efb` → `0x1d77a7` | Descriptor passed to CPU consumer after sort return; separate tail observes offset +4 read |
| Reuse | `0x1d7818..0x1d7823` | end reset to begin, storage retained; **new content epoch without new allocation** |
| Growth retirement | `0x2a070..0x2a0e8` | Old raw block freed; begin/end/capacity replaced. Future same address is not same generation |
| Thread-exit destruction / external aliases | Not closed | Must remain unknown; selected-path reconstruction is not a full lifetime proof |

The capture began after allocation/fill. Its address and size cannot recover the
missing birth event. The trace also does not include every thread or pending
kernel/driver writer. Nearby timing-counter atomics are not identified as a lock
on this array. No acquire/release pair protecting the array has been established.

## 3. Executable admission condition and what a future observer must establish

Implemented `scripts/cpu_admission_conditions.py` with independent results:

```text
eligible = semantics_pass AND memory_pass
```

Semantics requires a matching code generation, admitted input/path guards, exact
permutation on all those paths, continuation-state preservation, and preserved
fault/external-effect behavior. Allocation evidence cannot satisfy any of these.

Memory requires all of:

1. A verified allocation identity, generation, covered interval and content/lease epoch.
2. A **complete** access domain for that interval: readers, writers, free/remap,
   escaping aliases, new threads and any kernel/driver/DMA users.
3. Exclusive access and pinned lifetime from reading inputs through the **last
   output write**, not merely at dispatch and completion timestamps.
4. No consumer publication/return until commit completes.
5. A trusted provider: closed alias/ownership proof with runtime guards, or an
   enforceable complete-access lease. Observational logs are not a provider.

Dispatch creates a ticket bound to allocation/generation/range/epoch. Commit
rechecks the ticket, GPU completion and verified scratch result **while exclusion
is still held**. Two unlocked checks would have a TOCTOU race. On a pre-commit
failure, discard scratch and retain original CPU execution; do not write into a
freed/reused range. A live implementation also needs defined failure behavior
during commit; this checker does not implement one.

The checker is an executable specification, **not a live dispatcher or lease
provider**. Its positive unit tests supply synthetic trusted facts, not a certificate
for Wicked. Actual evidence yields no dispatch ticket:
[current-admission.json](../evidence/cpu-large-admission-20260925/current-admission.json).

### Required data and why it helps

| Needed information | What it can establish | What it cannot establish alone |
|---|---|---|
| Allocation/free/reallocation and raw/interior pointer mapping from before birth | Generation, lifetime changes, ABA rejection | Absence of aliases or competing accesses |
| TLS descriptor transfers and pointer publication through relevant callees | Candidate ownership/escape graph | Completeness when instructions, threads or calls are unobserved |
| Actual acquire/release/publication and outstanding async users | Candidate happens-before boundaries and first permitted consumer | Exclusion if the synchronization domain is not proved complete |
| Runtime checks of generation/epoch and proof assumptions | Whether an existing certificate still applies | A new proof of ownership or sort semantics |
| Complete enforced access barrier held through commit | Prevent access/free while replacing | Semantic equivalence of the replacement |

A feasible **limited** observer would record these lifecycle/transfer/synchronization
events, not millions of ordinary arithmetic steps. However, for its data to justify
admission it needs static closure of the observed domain, or enforcement on every
relevant access. A DBI observer that misses a new thread, syscall-visible pointer,
encoded pointer escape or asynchronous writer must invalidate the candidate.
Finite no-conflict observations cannot turn that unknown into a proof.

Simple page guards are not proposed as the missing lease: resetting PAGE_GUARD and
resuming a thread can open an unprotected window for other threads. Suspending a
sampled set of threads leaves new threads and outstanding asynchronous accesses
uncovered. A stop-the-world snapshot by itself is neither an ownership certificate
for subsequent frames nor a profitable production design.

**Therefore no new observational capture was launched.** With the existing
single-invocation diagnostic, the missing complete-access domain cannot be
established. This is a concrete limitation of this method, not evidence that GPU
execution is slow or that all automatic transfer is impossible.

## 4. Inner operations: economics before development

Neither inner operation was developed into a GPU kernel in this pass.

- **Index generation** is under a buffer-size-change branch. No complete counters
  or isolated useful-time sample exist for that branch. Per-frame frequency and
  cost remain unknown. Independent cells could be batched after allocation guards,
  but a buffer-creation cost is not assigned to every frame.
- **Bitmask expansion**: the existing short consumer tail contains **829 loop
  visits, all with mask 1**. This is a partial continuation, not 829 calls per frame.
  A separate historical source-assisted screen measured **two whole RenderMeshes
  calls per frame**, averaging **4.9143 ms summed wall time** over 424 samples after
  6 seconds. This includes batching, material/state work and submission; it is not
  the cost of the small bitmask loop. Sorting in the same screen totals 7.1379 ms
  per sample. These sums are not critical-path frame savings.

Pure bitmask-loop cost and full per-frame frequency remain unmeasured. A useful
batch would need multiple outer records, deterministic offsets, batch counters
and mapped-buffer publication. There is no justification yet for one GPU dispatch
per 1–16-word inner invocation. No claims of recurring gain are made for either
operation. Exact metrics, provenance and the filtered raw CSV are included in
[inner-economics.json](../evidence/cpu-large-admission-20260925/inner-economics.json).

## 5. Validation and reproduction

- 49 native differential cases, 1,074,241 records, zero mismatches/canary failures.
- 3 model regression tests against stored **native** output hashes.
- 9 independent-axis/ticket tests: generation reuse, revocation, publication,
  incomplete coverage, code/state failure, fence/result readiness and range wrap.
- Existing snapshot/CFG tests checked for regressions.
- Evidence replay verifies image, six static regions, short tail, raw parent
  timing excerpt and current rejection.

```powershell
python scripts/check-cpu-large-sort-native.py <study>/final-training/candidate-00 <study>/logical-plan/candidate-00/checked-functions.bin <result.json>
python scripts/verify-cpu-large-admission-evidence.py <study> <detector>/module-image.bin <resident-study>/screen.csv docs/evidence/cpu-large-admission-20260925
python tests/cpu_large_sort_tests.py
python tests/cpu_admission_conditions_tests.py
```

The original inputs remain in the previously committed evidence archives. No large
trace is duplicated. The old 57.61-second maximum training-frame gap is not claimed
fixed; offline replay does not measure renderer frame latency. The tracer remains
an explicit diagnostic tool.

**Current outcome:** a full-size exact-order computational model and executable,
separate admission conditions; no proven live-memory ownership, no certified
all-path replacement, no automatic GPU execution and no measured net gain.
