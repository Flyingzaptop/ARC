# CPU task contract refinement from `4017d03`

## Result

No repeated full trace, game launch, or GPU replacement. Reused the saved code,
input/output snapshots, short stopped traces, continuation and frame telemetry.
Added a conservative CFG constant audit, executable snapshot guards, a native
isolated original-code oracle and inner-loop analysis of atomic-containing parents.

**Live admission remains closed.** This report does not claim the requested
automatically replaced operation or a net performance gain. The missing mechanism
is allocation/alias/synchronization provenance through the replacement interval,
plus closed machine semantics/continuation for the useful large operation. Another
identical instruction trace cannot supply those guarantees.

Machine data: [semantic contract](../evidence/cpu-contract-refinement-20260924/semantic-contract.json),
[recomputed evidence](../evidence/cpu-contract-refinement-20260924/refinement.json),
[native oracle](../evidence/cpu-contract-refinement-20260924/bounded-native-oracle.json).

## Comparison and equal keys

The instruction-derived key is an **unsigned 64-bit integer**, ascending:

```text
load_u16_le(record + 8)
| (uint64(record[12]) << 16)
| (uint64(load_u16_le(record + 0)) << 24)
| (uint64(load_u24_le(record + 13)) << 40)
```

This does not compare floating-point distance directly. Bytes 4..7, bytes 10..11
and the high half of the first uint32 do not enter this projection.

The earlier inference assumed constant masks based on unique literal setters.
The new forward fixed-point analysis propagates **must** constants across branches
and loops; a missing definition, conflicting path or partial write loses the fact.
It establishes the required masks at all four previously inferred comparison sites
(`0x22374e`, `0x2237d0`, `0x229cd8`, `0x229d52`), with unsigned flag consumers.
It assumes Win64 preservation across calls. This closes those mask assumptions,
**not** all key provenance, all comparison sites or whole-sort correctness.

Recomputed on the real 65,344-record input:

| Property | Result |
|---|---:|
| Distinct keys | 11,405 |
| Equal-key groups with different full records | 8,536 |
| Positions differing from stable sort | 53,871 |
| Record multiset preserved | Yes |
| Output ordered by unsigned key | Yes |
| All position differences confined to equal-key groups | Yes |
| Visible-result difference proved | **No; unknown** |

Within tied groups only record bits 32..47 vary on this snapshot. They belong to
the instance identifier. The continuation already observed a read at offset +4,
RVA `0x1c9e94`. Thus these are not demonstrably dead payload bits.

The separately inspected reference source explains the consumer: it reads instance
ID and camera mask, emits ordered instance-pointer words into a GPU allocation,
and flushes batches based on mesh, LOD and stencil. Equal sort keys need not make
that entire computation commutative. A changed pointer sequence can still produce
the same image on a particular scene. The source-assisted control previously
demonstrated just such a case; it does not certify arbitrary consumers/render states.

The reference excerpts and file hash are saved in
[consumer-reference.json](../evidence/cpu-contract-refinement-20260924/consumer-reference.json).
Source was used only to independently interpret the existing discovery. It did
not choose a candidate, and source-to-current-binary correspondence is not proved.

## Restricted class and executable checks

The broad byte-invariant tie rule is: every group of equal keys contains identical
**full** records. Distinct keys are a special case. Then any ordered permutation
has the same output bytes, eliminating the downstream tie-order question.

For a much smaller path proposal, also require:

1. Exact reviewed code generation; aligned nonwrapping 16-byte array span.
2. 2..32 records.
3. First key is the unsigned minimum.
4. Byte-invariant ties as above.

The size branch selects insertion sort. The minimum-first sentinel prevents the
external move-helper branch and bounds the backward insertion scan. This removes
recursive partitioning, heap fallback and indirect move dispatch from this proposed
path. It does not establish the live allocation's ownership or all continuation state.

`cpu_sort_contract.py` executes the data checks on an owned snapshot, with a 4 MiB
budget. It deliberately returns `replacement_admitted: false`, even if data checks
pass. **These are offline preflight checks, not an installed live dispatcher.**
The real 65,344-record input fails the path-size, sentinel and tie checks.

The isolated native oracle executes the **saved original function bytes**, pinned
by SHA-256, on 744 deterministic cases across sizes 2..32, including unsigned
high-bit keys and identical duplicates. All byte outputs match the reference;
prefix/tail canaries remain intact. No application attachment, GPU or replacement
is involved. The test does not establish general ownership, full ABI state or net
benefit, and the small cases are not substituted for the real workload.

## Ownership and synchronization

Reference source has a thread-local vector, filled before sorting and consumed
synchronously into a **different** GPU upload allocation. Reuse happens on later
calls. This provides a concrete ownership hypothesis and distinguishes the CPU
sort array from its downstream mapped GPU output.

It is still insufficient for a binary replacement certificate:

- TLS storage does not prove no pointer escapes to another thread or asynchronous writer.
- `MEM_PRIVATE`, unchanged bytes, and disjoint observed argument spans do not prove ownership.
- A hash before/after GPU work cannot protect a freed/reused allocation or prevent a
  concurrent consumer from observing partial commit.
- Same-thread return/first-read observations do not establish the first global consumer.

The required next mechanism is **allocation-generation and alias provenance** from
the TLS vector's begin/end/capacity through callees and published aliases, including
free/remap and asynchronous writers. Admission needs either a closed ownership proof
with generation/epoch checks on each invocation, or an enforceable exclusive lease
covering snapshot through commit. The original synchronous return must not occur
before commit. Stopping a few observed threads or merely seeing no races is not a
substitute. Caller live-outs and exception/XSTATE behavior must also be closed over
the admitted continuations.

## Atomic parents: retain synchronization, investigate inner computation

The old whole-function interpretation was too coarse. The new inventory does not
reject an entire function because a trace encountered an atomic. It identifies
backedge intervals and reports atomics/calls **inside each interval**. This is an
inventory, not proof of a single-entry loop or absence of callee effects.

| Parent | Backedge intervals | No direct atomic | Also no calls |
|---|---:|---:|---:|
| `0x1d7050` | 8 | 5 | 2 |
| `0x1c9940` | 3 | 2 | 1 |
| `0x1d1060` | 3 | 3 | 0 |
| `0x244850` | 1 | 0 | 0 |

Two concrete inner operations:

- **`0x1d7250..0x1d72ae`: grid-index generation.** The nested loops emit six uint32
  indices per cell without array reads, calls or atomics. Allocation, zero fill,
  virtual buffer-creation callback and publication stay on CPU. The destination
  comes from a preceding allocation call; a size-comparison branch bypasses this
  work when the existing buffer size matches. Its allocation/consumer callback
  contract is unresolved, and the stopped trace does not show this path is hot.
  Treating it as recurring frame work would overstate its value.
- **`0x1ca130..0x1ca15b`: bitmask expansion.** A nonzero 16-bit mask produces up to
  16 packed words in ascending bit order. The loop also increments a stack batch
  counter and leaves live registers/flags; those effects cannot disappear.
  The surrounding consumer allocates a mapped GPU buffer. A useful transfer would
  likely need batching the outer records, but its dependencies and buffer lifetime
  are not yet closed. This is not rejected because of the parent's atomics.

Exact formulas, guard obligations and consumer boundaries are in the semantic JSON.
Observed `lock xadd` sites are retained ordered CPU effects; they are not assumed
to be mutex acquisition merely because they are atomic. The third/fourth parents
remain candidates for callee-effect closure, not globally marked untransferable.

## Maximum training-frame delay

All recorded intervals remain in the statistics, including scene loading. These
are CPU-side **Controlled Present intervals**, not display FPS.

| Capture | Maximum interval, ms | p99 nearest-rank, ms |
|---|---:|---:|
| Full sort | **57,608.33** | 47.84 |
| Candidate 01 | 2,338.13 | 24.11 |
| Candidate 02 | 3,119.93 | 39.36 |
| Candidate 03 | 2,347.27 | 26.01 |
| Candidate 04 | 2,294.11 | 22.96 |
| Candidate 05 | 3,403.54 | 37.32 |

The full-sort trace's render interval alone reaches 57,599.68 ms. The other maxima
include loading and are not attributed wholly to instrumentation. Low average or
p99 values therefore cannot certify training playability: the complete instruction
trace is unsuitable for interactive training, despite bounded memory and clean stop.
Keep it as an explicit diagnostic. The next capture should observe allocation,
publication, synchronization epochs and selected slice boundaries, not repeat all
10.8 million checkpoints. No tracer budget was relaxed or tracer removed here.

## Validation and reproduction

- 7 snapshot-contract tests: ties, unsigned keys, corruption, span bounds, path guards.
- 6 CFG tests: dominance, join, loop redefinition, partial writes, calls, unknown edges.
- 744 isolated native original-code cases, zero output/canary failures.
- Existing contract tests and discovery tests checked for regressions.

```powershell
python scripts/refine-cpu-task-contract.py <unpacked-study-root> docs/evidence/cpu-contract-refinement-20260924
python tests/cpu_sort_contract_tests.py
python tests/cpu_cfg_constants_tests.py
python scripts/check-cpu-bounded-sort-native.py <study-root>/final-training/candidate-00 <oracle-output.json>
```

The prior raw archive remains the input evidence; new output records input hashes.
No historical rendering matrix was repeated. **Net GPU-replacement gain remains
unmeasured**, because live replacement was not admitted. Source-assisted resident
Wicked gains are not included in this result.
