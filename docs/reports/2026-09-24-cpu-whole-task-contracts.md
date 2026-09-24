# Whole CPU-call recovery in training mode — 2026-09-24

## Result

Recovered a **complete top-level invocation**, not a two-record swap: an in-place
ordering operation over **65,344 opaque 16-byte records / 1,045,504 bytes**.
Selection came from `b810191` detector output, without target symbols or predefined
Wicked addresses. The observer captured entry, matching return address/RSP, the
executed callees and ordered user-mode memory operations. Input/output snapshots
preserve the same record multiset.

**A safe automatically replaceable contract is NOT complete. No GPU replacement or
performance gain is claimed.** The machine-readable contract keeps required unknowns
closed. This stage materially expands the evidence; it is not an acceptance claim for
automatic offload.

## Correctness fix

`a7e29cf` moves observer budget accounting ahead of the failed-context branch. Every
attempt consumes its elapsed time, including failed `GetThreadContext`; suspension
is undone by the existing RAII scope before the budget decision. Native tests cover
single failed attempts, accumulated failed attempts, mixed outcomes and invalid time.
Windows module-name normalization also now uses `PureWindowsPath` for Linux replay.

## New capture channel

- A hardware execution breakpoint identifies the selected function entry. No game code
  bytes are patched. Entry selection is grouped by chained Windows unwind metadata;
  a prologue fragment is not mistaken for the entire logical function.
- Trace mode records a selected complete invocation and callees, then a bounded caller
  continuation. Boundary mode observes entry/return without stepping the body.
  Consumer mode observes the normal call, then follows the continuation.
- Training permits visible slowdown. It streams losslessly encoded context changes
  through a fixed ring. Register-only instructions can be omitted from the log only
  using a byte-verified machine-code plan; memory operations, calls, returns and branches
  remain. Offline flow checking accounts for the omitted straight-line instructions.
- Limits: ring 32,768 records, bounded encoder, 4 MiB per proposed input/output snapshot,
  at most 20 million recorded events, **2 GiB stream**, and at most 60 seconds of trace.
  The selected capture allocated **58,515,456 bytes** of explicit buffers.
  Static/context bookkeeping is additional and small; whole game memory is not ARC cost.
- Explicit stop removes new tracing and owned debug state. A handler is kept loaded
  while a delayed trap could still arrive. The stop fixture completed its original work,
  reported reason 8, and `trap_cleanup_pending=false`.
- The exception observer preserves Win32 LastError/NT status. Budget decisions in the
  application exception handler use integer arithmetic, avoiding accidental FP traps
  when the application's precision exception is unmasked. Dedicated tests cover this.

No GPU scheduler, knowledge-sharing/file exchange system or new engine adapter was built.
Input/output binary files below are ordinary capture evidence, not a new knowledge protocol.

## Whole-operation evidence

The final whole-call trace recorded **10,812,118 checkpoints**, with
**23,969,261 verified register-only instruction checkpoints
omitted**, and reconstructed **7,632,582 memory events**. There are zero
unresolved memory effects or flow gaps on the recorded call. Exact byte coverage was
reconstructed from the stream using **413,184
bytes of bitmaps**; it is not limited by the preliminary interval tracker.

Four straight-line comparison sites yield the same local 64-bit bit-projection hypothesis.
It has **11,405 distinct keys** on this input and agrees with the observed
nondecreasing output. Constant dominance, all comparison sites and whole-algorithm
semantics are not thereby proved; those limitations are retained in the JSON.

A stable sort by that key differs from the actual CPU output at **53,871
record positions**. Thus key ordering plus record permutation is insufficient to justify
substituting an ordinary GPU stable/key sort. This is a concrete counterexample, not an
assertion that no faithful GPU implementation could be developed.

The observed same-thread consumer reads at **RVA `0x1c9e94`**, offset **+4** in the result
span, in another region already found by the detector. The longer continuation retains
register-alias-aware liveness masks. In the observed path it requires the ordinary
nonvolatile registers/vector lows and stack state; volatile integer outputs are overwritten
before use. This does not prove every unobserved caller path.

A concurrent entry into the same function with a different argument span is recorded.
These observed spans are disjoint. **Other threads' complete memory traffic is not
captured**, and this fact does not prove ownership, absence of races, or the globally first
consumer in future invocations.

## Candidate outcomes

| Candidate / entry RVA | Whole return | Detailed result | Decision |
|---|---|---|---|
| candidate-00 / `0x223550` | True | 10,812,118 checkpoints; atomic stop=False | exact_tie_order_and_global_ownership_not_proven |
| candidate-01 / `0x1d7050` | True | 26,504 checkpoints; atomic stop=True | atomic_synchronization_outside_pure_transform_class |
| candidate-02 / `0x1c9940` | True | 827 checkpoints; atomic stop=True | atomic_synchronization_outside_pure_transform_class |
| candidate-03 / `0x1d1060` | True | 706 checkpoints; atomic stop=True | atomic_synchronization_outside_pure_transform_class |
| candidate-04 / `0x230c10` | True | 52,016 checkpoints; atomic stop=False | unclosed_external_effects_and_memory_contract |
| candidate-05 / `0x244850` | True | 14 checkpoints; atomic stop=True | atomic_synchronization_outside_pure_transform_class |

The four atomic cases are allowed to finish **on the CPU** after detailed tracing stops;
their observed return is not presented as a complete body trace. The other nonsorting
candidate retains unresolved external effects and a non-array memory contract. No
candidate is discarded solely because another one failed.

## Contract status

| Property | Status |
|---|---|
| Selected entry, normal return and source code identity | Checked for the recorded generation/invocation |
| Input/output register state, flags, MXCSR | Captured; reported XSTATE captured in the continuation study |
| Addresses, widths and order on the selected sort invocation | Observed completely for supported user-mode effects |
| Dataset size and byte span | Observed 65,344 × 16 bytes; future bindings still require guards |
| Read/write dependencies | Raw sequence retained; bounded online summaries are not universal proofs |
| Exact permutation and first same-thread consumer | Observed; tied-key counterexample retained |
| Every path, indirect jump target and caller live-out | **Unknown / not proved** |
| Global ownership, all-thread access and synchronization | **Unknown / not proved** |
| Executable per-invocation guards and equivalent GPU implementation | **Not implemented / not admitted** |

The static audit covers four verified logical functions for the sorting candidate and
finds four unresolved indirect-control sites. It is a structural audit, not an equivalence
verifier. Larger renderer routines expose additional calls and synchronization effects.

The present method's limit is now **proof**, not capture latency: extending the same trace
cannot establish all future paths, global ownership, or permission to change tied order.
Closing the gate requires a verified supported-operation model plus executable guards;
creating a GPU kernel from the observed examples alone would violate the requested
correctness boundary. The scope of supported exact replacements therefore remains empty.

## Costs and validation

The non-stepped outermost boundary sample processes **65,536 records** and spans
**3.5101 ms**. This is per-invocation wall time, not frame saving. It includes scheduling
and debug delivery. Its `GetThreadTimes` delta is 15.625 ms, exceeding wall time because
of accounting granularity; it cannot be used as exact useful CPU time. Waiting versus
Ready time is not resolved. Worker durations are never summed into a frame gain.

The detailed 65,344-record training invocation took **57.592 s**, including **5.353 s**
measured inside the observer handler. The remaining time includes exception delivery,
original work and scheduling; it is not presented as useful computation. This slowdown
is confined to the explicitly authorized training run. There is no performance-mode win.

Validation: Release builds succeeded; context-budget and lossless-codec native tests
passed; 9 discovery tests and 5 contract/liveness tests passed. The native fixture checked
300 calls, output equality, MXCSR and LastError preservation. Unmasked FP control and
mid-capture stop were tested separately. Final captures exited normally, cleanup-pending
was false, and the prior Wicked executable was restored. No commercial games or old
benchmark matrix were run.

## Files and reproduction

- [Machine-readable contracts and decisions](../evidence/cpu-contract-20260924/contracts.json).
- [Selected contract](../evidence/cpu-contract-20260924/candidate-00/contract.json).
- [Tie-order counterexample](../evidence/cpu-contract-20260924/candidate-00/tie-order-counterexample.json).
- [Raw archive manifest and split-part hashes](../evidence/cpu-contract-20260924/manifest.json).
- [Reproduction commands](../../benchmarks/cpu-contract/README.md).

GPU-transferred work and eliminated CPU invocations: **zero**. The manual Wicked
resident-adapter gain is not included in this result.
