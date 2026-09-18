# Stage 15 closure work — 2026-09-18

**Status: lifecycle and independent family validation complete; Stage 15 acceptance
remains FAIL on workload complexity alone. Stage 16 is not started.**

Subsequent user decision (2026-09-18): defer the 81/128 complexity gate and proceed
with Mega D (Stages 16, 17 and 20). The statement above records closure-run status;
its formal FAIL and original artifacts remain unchanged. Larger-workload validation
is open technical debt, not a passing gate.

## Lifetime protocol

Implemented explicit command-list and queue retirement across the event protocol,
ResourceGraph, RuntimeEventBridge, native D3D12 adapter and pinned Wicked teardown.
Command destruction preserves already-submitted work. Queue retirement refuses
unresolved controlled submissions, polls the bound completion fence, and then
removes native/runtime/graph records and unshared fence bindings. Reused native
pointers receive new IDs. Swapchain back buffers now notify ARC before forced
release after the renderer's GPU wait.

Live per-resource queue history is also retired, with conservative invalidation
of affected observation checkpoints. Offline history remains explicit; binary
trace schema is now 5. Memory is bounded by configured retained history, live
application objects and genuinely outstanding controlled work, provided the
cooperative host sends lifecycle notifications.

Validation: 10,000 logical lifetimes, 1,000 COM pointer re-registrations, shared
fence ownership, real WARP submission/completion, and rejection of premature
queue retirement. Full Release: 55/55 including 17 GPU tests. Debug core/stress:
38/38. See [history contract](RUNTIME_HISTORY.md).

## Independent semantic evidence

Added an evaluation-only catalog of pinned Wicked allocation owners. Names never
enter core features or controller inputs. The external evaluator recomputes
coverage/precision from per-resource predictions, rejecting inconsistent summary
numbers. A dependency-boundary test prevents audit-label dependencies in core
and host code. Native GetDesc flags are checked against captured graph metadata.

The scope is six allocation/use families, **not** fine distinctions such as
shadow versus camera depth, temporal history, or geometry versus general shader
data. Confidence is still an uncalibrated heuristic. The fine-class confusion
matrix and unknown predictions are retained. See [catalog and gates](SEMANTIC_AUDIT.md).

On the corrected `09dc94f` run: 27 labeled resources, all six families; 24 correct
out of 24 classified (100% precision), three abstentions (88.89% coverage), zero
native metadata mismatches and zero high-confidence wrong family predictions.
The unknowns are the instance/material/geometry shader-read buffers. This does
not establish generalization to other renderers or fine-subtype accuracy.

## Hardware evidence retained

| Source / run | Outcome |
| --- | --- |
| `bb75dd9` / `20260918-185953` | FAIL: scene selection re-enabled VSync and capped light scenes at 144 Hz; endpoint population gate also failed. Audit was 23/24 correct with three unknowns. |
| `09dc94f` / `20260918-191800` | Stage 14.5 PASS: GPU p50 2.315 -> 2.094 ms, misses 60% -> 35.9%, 2/5 scene wins, 16 actions across three domains, clean graph/backend and full recovery. Stage 15 failed only its endpoint resource-population check (120 versus 128). |
| `8278c25` / `20260918-193008` | Stage 14.5 PASS again: GPU p50 2.250 -> 2.060 ms, misses 59.9% -> 33.8%, 2/5 scene wins. Independent family audit and all scene/safety gates pass. Stage 15 remains FAIL solely because the largest measured baseline live population is 124, below 128. |

These runs are immutable under their `results/stage15-wicked-<stamp>` branches.
No failed run was replaced. Actual nonzero Present sync intervals now invalidate
both official and three-arm measurements; initialization alone is not trusted.
Automated runs use the renderer's alwaysactive option to avoid focus pauses.

## Population measurement correction

Acceptance schema 5 selects the largest complete measured **baseline** population
by count alone and records all baseline/adaptive populations plus the selected
frame and scene index. The evaluator requires an actual matching capture; an
invented population or wrong frame fails. The end-adaptive count remains in the
report. This replaces choosing the last, relatively lean scene as evidence for
the whole workload, and avoids counting stale destroyed identities.

The threshold stays 128, and the other numeric acceptance thresholds remain
unchanged. If no measured baseline capture reaches 128, the gate still fails.
This change to population selection is explicit; it is not a claim that the
earlier endpoint gate passed.

The complete baseline populations in the population-witness run were
122, 124, 124, 123, 121. The run honestly remains FAIL; no snapshot was invented
and the threshold was not lowered. A possible specification change is to require
128 distinct resources actually used over all measured baseline windows, with
per-window IDs and an independently verified union. That is a different meaning
of complexity and was submitted to the user as an explicit decision. The user
approved that definition: acceptance schema 6 requires the independently verified
union of actually used resource IDs across five complete measured baseline
windows to contain at least 128 members. Implementation preserves each window's
IDs and checks its count against the corresponding active-resource signature.
Historical schema 4/5 failures remain failures; a fresh schema 6 run is required.

## Final schema 6 run

Source `cd13c3516bbd986ef3b60d54e11f4406c046d428`, pinned Wicked
`0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`, native 1920x1080,
60 seconds baseline plus 60 seconds adaptive.
[Immutable raw run and acceptance](https://github.com/Flyingzaptop/ARC/tree/results/stage15-wicked-20260918-195042/results/stage15-wicked/20260918-195042).

Stage 14.5 PASS: GPU p50 2.320384 -> 2.079744 ms (-10.37%), p99
5.161534 -> 5.052488 ms; deadline misses 59.9689% -> 33.9355%.
Two of five scenes win, 14 actions span three domains, six effects are learned,
and full-quality recovery passes. Actual VSync Present count is zero.
Independent audit again gives 24/24 correct classified families, 27 samples,
all six families, three abstentions, and zero metadata mismatches.

Stage 15 FAIL: 28 of 29 gates pass; only `complex_resource_population` fails.
Independent HashSet recomputation of the raw per-window IDs confirms **81**, not
128, unique used resources. All five histories are complete, and ID-list lengths
agree with their independently computed scene activity counts:

| Baseline window | Used IDs | Newly added IDs | Cumulative union |
| --- | ---: | ---: | ---: |
| Model | 66 | 66 | 66 |
| Shadows | 68 | 2 | 68 |
| Water | 78 | 12 | 80 |
| Volumetric | 69 | 1 | 81 |
| 65k Instances | 66 | 0 | 81 |

The collector compares submitted read/write counters to each window checkpoint,
including retained destroyed records; it does not filter the union to currently
live objects. No union/counting error was found. The measured scenes share most
observed resources. This is an observed-use lower bound: descriptor creation and
bindless heap membership alone do not establish actual shader access. Unobserved
bindless usage must not be counted simply to satisfy the gate.

The approved criterion is implemented, but this workload has not demonstrated it.
Closing this gate requires a representative workload with at least 128 provably
used resources, or substantiated additional use instrumentation. Neither has been
validated here. The threshold remains 128; Stage 15 is not marked complete.

## Counterbalanced OFF / observer / adaptive comparison

Source `8278c251da045e9c9d17fdbcaf97fcd7aeebcc27` (before the additional
resource-ID telemetry), same pinned renderer. Three repetitions, rotated arm
order and scene offsets, 20 seconds per arm, separate calibration, hook timing
instrumentation disabled. Every run has zero VSync Presents and passes the
comparison validity checks. Ratios use equal scene/repetition weighting.

| Scene | Observer frame interval vs OFF | Adaptive frame interval vs OFF | Adaptive GPU p50 vs OFF |
| --- | ---: | ---: | ---: |
| Model | +7.09% | +7.95% | +0.65% |
| Shadows | +1.04% | -2.11% | -3.22% |
| Water | +0.81% | -10.89% | -10.59% |
| Volumetric | -0.04% | -17.53% | -17.98% |
| 65k Instances | +0.71% | +1.95% | -0.50% |
| Equal-weight mean | +1.92% | -4.13% | -6.33% |

The JSON's `cpu` metric measures wall-clock frame intervals, **not busy CPU time**.
OFF retains the shared harness and trivial exported hook branches. Model regresses;
the result is not a universal speedup. This comparison does not establish image
quality equivalence or independently validate recovery. Recovery is covered by
the separate official run.

[Comparison JSON and local test logs](https://github.com/Flyingzaptop/ARC/tree/results/stage15-closure-20260918/results/stage15-closure/20260918).
Release 55/55 and Debug core/stress 38/38 passed after runtime/lifecycle changes;
the final bootstrap rebuilt the integration and passed its 17 deterministic tests.
Later collector/evaluator helper checks passed as well. These local configurations
are recorded separately rather than presented as one run of every configuration
at the final SHA.
