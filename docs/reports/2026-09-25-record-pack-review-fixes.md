# Review fixes and source-assisted record-package screen

Continues `00fe4a2`. The three selector defects are fixed. Useful work is now
measured at the selected loop, rather than inferred from the earlier 5.32 ms parent.
The first CPU-materialized packet route is screened out before GPU development;
the selector advances to other candidates. **This is not a GPU replacement or a
measured game speedup.**

## 1. Selector corrections

### Research is not replacement enablement

A feasible map/scan/reduction hypothesis may enter **bounded research with unknown
GPU and exchange cost**. It need not already have a GPU implementation. Research
and demonstrated economics are separate fields. Neither grants live replacement
permission.

Research is bounded to three attempts per material context. Capture plans cap
instruction research at 32,768 events / 100 ms; boundary observation retains its
64-event / 250 ms cap. The runner records attempts in the existing local measurement
ledger using atomic file replacement. This is not a pattern-exchange service.

### No overlap double counting

Positive economic evaluation uses a directly measured `replacement_total_ms` with
scope `input_ready_to_all_consumers_ready`, compared to the corresponding original
work. Any remaining CPU computation needed to produce the outputs/counters belongs
inside that replacement interval. Unchanged downstream consumers are outside both
intervals; their cost must be reported separately.

`gpu_ms`, upload, return and fence wait are diagnostic components; **they are not
summed** to obtain total cost. Missing end-to-end time remains unknown. An explicit
measured mandatory-input-preparation lower bound may reject a particular route,
but can never establish positive benefit. It is labelled as a screening bound,
not an executed replacement duration.

### Rejections do not reappear without changed conditions

Both cheap and detailed recommendations, and the capture-plan builder, skip rejected
candidates. Rejections/attempts are scoped to code identity, proposed algorithm,
device/driver, batch class, residency and remaining CPU contract. Sample counts or
new observation timestamps do not reset them. A material change such as genuinely
GPU-resident inputs permits a new bounded investigation.

The selector can consume the same saved inner-loop context and measurement file
as the screen. Thus advancing to a discovered inner interval produces an actual
capture plan, not merely a different name in the report. No new instruction trace
was run in this stage.

## 2. What was measured, and what remains manual

Candidate selection remains automatic. To obtain useful scope measurements, the
owned Wicked fixture was **temporarily source-instrumented**. Association between
the discovered binary interval and source, and the packet design, were reviewed
manually. This is a control experiment, not an engine adapter shipped in ARC.

Instrumentation separately times:

- the original outer record loop;
- every nested `batch_flush`, subtracted from that loop interval;
- all retained `batch_flush` calls, including the final one;
- gathering/materializing the proposed GPU input packet.

The measured slice contains record/object reads, dither/filter evaluation, group
changes, output offsets/counts/flags and packed-instance writes. Deliberate draw/API
work inside `batch_flush` is excluded. The result is **isolated elapsed work time**,
not OS-scheduler-separated running time. Scheduling and memory stalls remain in it.

Preparation measurements allocate/reserve a packet and gather the required fields.
They run after the original loop, so its cache warming favors preparation. Logging,
one-time evidence-file writes and copying the captured output are outside the
reported loop/preparation intervals. They can still perturb the application; this
is not a frame-performance comparison.

Each process ran for 15 configured seconds with a 45-second external limit. Five
short runs were made; the first ten large-batch frames and the last incomplete
frame were excluded uniformly from steady statistics. All raw rows are retained.
Logging is capped at 2,048 rows; reaching the cap makes frequency analysis fail.
Packets are capped at 131,072 records. Original EXE and source bytes were restored.

## 3. Frequency, size and useful work

All steady windows have **two calls per frame**, **65,344 records and 65,344 output
words per call**, **one group**, and no forward-light AABB reduction.

| Run | Complete measured frames | Original loop minus nested flush, median ms/call | Packet preparation, median ms/call | Paired headroom before any GPU work, median ms/call |
|---|---:|---:|---:|---:|
| timing-a | 225 | 5.6201 | — | — |
| prepare-a | 274 | 2.5781 | 2.8545 | **−0.2549** |
| prepare-b | 374 | 2.1603 | 2.4898 | **−0.3094** |
| timing-b | 345 | 3.0545 | — | — |
| gate confirmation | 427 | 2.2286 | 2.4892 | **−0.2517** |

Retained flush work is about **0.029–0.043 ms/call** in these runs. Allocation,
descriptor lookup, submission and other caller work are preserved; they are not
credited as removed CPU work. The earlier parent 5.32 ms is not used in any economic
decision here.

Between-run baseline variation is substantial. The negative screening decision is
based on the paired original/preparation intervals, not on combining the slowest
baseline with the fastest preparation. Preparation exceeds the original slice in
405/548, 647/748 and 635/854 paired calls respectively. These measurements support
an **unpromising initial estimate for this packet route**, not a proof that every
counterfactual GPU-resident design must lose.

## 4. Inputs, outputs and remaining consumers

The tested packet is 48 bytes per record:

- Original 16-byte record: mesh, instance ID, half distance, camera mask, LOD override,
  sort fields.
- Object transparency, fade distance, radius and alpha reference.
- Object LOD and stencil reference, plus padding.

At steady N=65,344, preparation writes **3,136,512 bytes per call**. Packed output
is **261,376 bytes** here. Neither GPU upload nor readback nor GPU execution has
been measured for this rejected route; their costs are not invented as zero.

The packet covers the pure-loop live-ins only in the tested non-forward class.
Forward-light AABB data and exceptional numeric inputs are rejected by the replay
analyzer. The three saved packet snapshots were captured earlier during loading at
N=65,536, not confused with the steady N. The independent packet model reproduces
all **196,608 captured output words exactly**. This validates those snapshots,
not arbitrary live-program equivalence.

The parallel proposal remains per-record evaluation → counts/group starts →
segmented prefix offsets → stable scatter. Dither flags can be combined only with
matching semantics. The CPU still needs group counts, data offsets, mesh/LOD/stencil
selection and alpha-test flags before its retained flush/command construction.
Thus keeping packed words on GPU alone does not eliminate all CPU dependencies.

Counting, offsets and scatter could share GPU intermediates without intermediate
returns. But this particular route first repeats the scattered CPU reads and
materializes 48-byte records. Its measured preparation already consumes the budget
in the paired screen. A route using existing GPU-resident object data would be a
**different context and hypothesis**, requiring a proved resource mapping and new
cost measurements; it is not silently substituted for this result.

The preparation bound is implementation-specific: it measures this 48-byte layout
and gather/allocation strategy. A narrower layout, pooled implementation or reuse
of an existing GPU representation changes the contract and permits reconsideration.
It is not a theoretical lower bound on every possible implementation of packing.

## 5. Automatic progression and decision

The recorded selector progression is:

```text
outer record package 1c9e90–1ca188
    -> grid-index generation 1d7260–1d72a3
    -> narrower mask expansion 1ca130–1ca15b
```

The first route is rejected by its measured mandatory-preparation bound. No GPU
scan/scatter prototype was built, because this initial route assessment does not
promise savings even before transfer, computation, waits and remaining CPU work.
Its full replacement duration is still unknown; no negative GPU benchmark is claimed.

For the next candidate, source inspection identifies a necessary ocean-enabled
guard before grid generation. The control probe observed that guard **false on
854 calls / 427 complete measured frames** in the tested scene. Under that
source-assisted control mapping, the selected grid path has no recurring work in
this window. This is not a missing-entry observation, not a claim about every game,
and not a proof that all buffer creation is cheap. The selector rejects it for this
context and advances again.

The narrower mask-expansion candidate is now proposed for bounded research with
unknown economics. Its removable work and smaller input contract need a separate
measurement; the outer loop's time is not assigned to it. An actual capped capture
plan was generated, but not executed as another instruction trace in this pass.

## 6. Checks and artifacts

19 selector tests pass: the existing 13 plus targeted unknown-economics, overlapping
time, cached-rejection/context-change, attempt-budget, persistent-ledger and
preparation-bound checks. The full historical matrix was not rerun.

- [Paired source-control analysis](../evidence/record-pack-review-20260925/control-analysis.json)
- [Confirmation and necessary-guard analysis](../evidence/record-pack-review-20260925/gate-analysis.json)
- [Measured feedback, explicitly labelled source-assisted](../evidence/record-pack-review-20260925/parallel-measurements.json)
- [Selection after rejecting the package](../evidence/record-pack-review-20260925/selection-after-pack.json)
- [Selection after the inactive grid path](../evidence/record-pack-review-20260925/selection-after-grid.json)
- [Raw controls, snapshots, tested binaries and patches](../evidence/record-pack-review-20260925/raw-controls.zip)
- [Archive hashes](../evidence/record-pack-review-20260925/archive-manifest.json)

Raw CSVs, snapshots, instrumentation patches, source/binary identities and
restoration records are archived alongside the report. Exact-sort development
remains stopped; no commercial game, full instruction trace, live replacement or
pattern-exchange infrastructure was involved.

Recompute the evidence and automatic progression:

```powershell
python experiments/record-pack/analyze_control.py <unpacked-archive>/control <control-analysis.json>
python experiments/record-pack/analyze_control.py <unpacked-archive>/gate <gate-analysis.json>
python experiments/record-pack/feed_screen.py <original-probe> <saved-logical-plan> docs/evidence/record-pack-review-20260925
python tests/cpu_parallel_screen_tests.py
```

`measurement-bindings.json` makes the manually reviewed source associations explicit;
the feedback producer refuses to attach those measurements to a different automatic
winner. Current condition metadata identifies the i7-11800H / RTX 3060 Laptop host
and driver 616.64. No per-frame hardware-counter attribution is claimed.
