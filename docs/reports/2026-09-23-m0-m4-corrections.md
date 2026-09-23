# M0–M4 corrective review pass

Base: `3413c2b`. M0 is accepted. M1–M4 remain pending re-review; neither the old
package nor these local checks establish whole-game acceleration.

| Review item | Correction | Targeted evidence |
|---|---|---|
| Protection invalidates everything forever | Target process and page range are checked. Restored code receives a new publication generation. Stale translated callbacks request rediscovery. | Unrelated page: zero invalidations. Own page and unchanged RX protection: fresh active generations and resumed execution. |
| First 64 regions permanently occupy slots | Fixed-size candidate/observer pools use recent activity and aging. Callbacks carry identity/generation, so reused slots cannot run a different region. Evicted previously admitted code can return. | 210-region fixture; late hot code executes, and the first region returns after eviction. 149 replacements in the return case. |
| Successful capture erases other novelty | Reusable exact-key tickets, per-capture completion, retirement of obsolete pending generations; CPU capture has no phantom bootstrap ticket. Spacing and failure backoff remain. | +2s/+60s deterministic cases, more than 300 generations, concurrent deduplication, collision gaps, cancellation, and actual two-region native study after 1.2s. |
| Passive PSO data lost | Bounded owned root/compute/graphics/stream snapshots, reserved before copying, replayed off the creation hook. COM and pointer-bearing payloads remain owned through replay. | The only output-capable compute PSO is created while passive and reaches preparation after resume without recreation. Pixel/stream descriptors and released app references are included. |
| PID reuse contaminates ETW ranking | Selected process start is unique within explicit clock uncertainty; first end is immutable. Later owner events are excluded across samples, modules, scheduling and presentation. Ambiguous starts decline. | Same-name previous owner and different subsequent owner; exactly three target samples, no `other.exe` region. |
| Runtime cost unused | `RuntimeCost` invokes `ProfitEstimate` on interleaved original/specialize/memo/incremental spans. Guard misses and redirects remain in the samples; timer, tracking and cold-admission costs are charged. Trials are bounded and policy is periodically revisited. | Native auto collects nine samples per action and rejects the expensive alternatives. Positive and negative selection are separately covered by a deterministic cost-model oracle. |

The native arithmetic fixture measured approximately 200 ns for the instrumented
original region and 2,000–2,200 ns for alternatives. Auto selected original after
its bounded trials. These are **within-DynamoRIO region measurements**, not frame
times, no-ARC timings or evidence of FPS improvement. Telemetry separates trial
executions from retained-policy executions and labels the last-thread snapshot.

Additional regressions found during integration were corrected:

- The DynamoRIO private loader left a dynamically initialized capture budget at
  zero. The CPU client now explicitly constructs it before registering callbacks;
  native output verifies 100 ms / 100,000 events / 8 MiB and two completed studies.
- Per-basic-block instrumentation context is not dereferenced by trailing metadata
  callbacks after it is freed.
- Cost calculations use a private masked MXCSR scope and restore application FP
  state. A fixture with unmasked precision exceptions passes the full state oracle.
- Session counters survive candidate replacement; retained-slot counters remain
  separately scoped by generation.

Native state checks include GPRs, flags, SIMD, MXCSR, x87, stack and the unsupported
memory-side-effect fallback. Only affected budget/model/parser checks, the focused
CPU lifecycle/auto/study checks, and passive-PSO native check ran. No commercial
game or full historical Cauldron/Wicked matrix was run.

Raw local evidence:
`C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/m0-m4-corrective-final-verified/`.
Source/binary fingerprints and results: `docs/evidence/M0_M4_CORRECTIONS.json`.

Remaining limits: main-EXE register MOV/LEA CPU domain; unsupported/oversized
passive GPU captures explicitly decline; CPU and DX12 launch modes remain
separate. A fresh elevated ETW provider capture and real-game usefulness remain
unverified. Separate per-thread cost histories survive eviction from the four data
cache slots. No process-wide overhead bound is claimed. These corrections do not
declare M1–M4 accepted.
