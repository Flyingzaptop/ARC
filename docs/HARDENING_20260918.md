# Runtime and measurement hardening — 2026-09-18

**Subsequent correction:** CPU profiling identified a per-frame swapchain
recreation bug in the shared harness, including OFF. It is now fixed; see
[root cause and measurements](CPU_STUTTER_FIX_20260918.md). The performance
numbers below remain historical evidence, not valid estimates of ARC overhead.

Current status: fixes published; Release 52/52 and Debug 35/35 pass. Latest
official GPU run passes every Stage 15-specific gate, but fails underlying
Stage 14.5 performance acceptance. Final comparison was interrupted by device
loss in the OFF arm; GPU work is stopped pending a laptop restart.

Source baseline: `2b7d2bc49972f69444a720985e537ed057148437`.
Implementation: `fcab25d781daf1be0e991ceff8bb5dcba82fbafd` on
`codex_den/runtime-hardening`.
Bootstrap follow-up: `b92c5acb23ac9b52ca8d24eed41ef5dcc42c3109`.
Read-only bookkeeping and semantic snapshot follow-up:
`de1e1061f35246c0bb52720a6679fc6f20a223b5`.

## Changes and evidence

1. Test executables explicitly retain assertions in every configuration. A new
   control test failed in the original Release configuration with
   `Assertions were disabled in a test executable`, then passed after the fix.
   Production libraries retain their normal optimized Release configuration.
2. Wicked command-list summaries retain both read and write presence instead of
   letting a write erase all read evidence. Portable regressions cover both
   access orders and repeated single-direction accesses. This is a summary,
   not an ordered dependency trace.
3. Live graph histories have configurable limits and cumulative workload
   counters. Checkpoint deltas survive submission/copy pruning; incomplete
   resource history is explicitly rejected. Tests compare bounded versus full
   history, exercise 10,000 resource lifetimes and descriptor reuse, and verify
   rolling-window recovery. Offline tracing retains full history by default.
4. All recurring native Wicked hooks have optional, constant-storage timing
   histograms including mutex wait. Comparisons disable this instrumentation to
   avoid perturbing hot hooks. Present/tick timing is no longer presented as the
   total observation cost.
5. Recurrence alone no longer generates `PersistentHistory`. Confidence is
   explicitly a heuristic score; semantic coverage is not classification
   accuracy. Acceptance requires explicit provenance/completeness flags and
   finite bounded numeric evidence. Existing numerical acceptance thresholds
   are unchanged.
6. A separate repeated OFF/observe/adaptive experiment uses one independent
   calibration, counterbalanced process order, matched rotating scene order and
   equal per-scene weighting. OFF creates no host adapter, but retains common
   harness/profiler and trivial callback overhead. It does not validate visual
   equivalence or replace the official recovery/Stage 15 gates.

## Local verification

Windows/MSVC 19.50, NVIDIA GeForce RTX 3060 Laptop GPU (plus Intel UHD Graphics).

| Check | Result |
| --- | --- |
| Release deterministic/core + stress tests | 35/35 PASS |
| Debug deterministic/core + stress tests | 35/35 PASS |
| Release D3D12 GPU labs and benchmark smoke suite | 17/17 PASS |
| PowerShell validation regressions | PASS |
| PowerShell script syntax | PASS |
| Official bootstrap deterministic regressions | 14/14 PASS |
| Pinned Wicked integration build | PASS |
| Official Stage 15 GPU acceptance | FAIL (details below) |

After the `de1e106` follow-up, the full Release suite passed **52/52** (including
17 GPU tests); Debug deterministic/core + stress passed **35/35** again.

GPU smoke results are correctness/integration evidence, not a claim of a
statistically significant speedup. Official Wicked and comparative measurements
are recorded separately with their exact source and raw artifacts.

The first official bootstrap stopped before renderer execution: Microsoft
Defender quarantined `arc-adaptive-quality-tests.exe` from the `/MT` build
(`Trojan:Win64/CymRan.ACY!MTB`). The same source test passed in the standard
Release CRT configuration. No Defender settings/exclusions were changed and no
quarantined executable was restored. The bootstrap now runs its unchanged test
selection in the standard CI CRT configuration and builds only the integration
libraries with Wicked's required static CRT. The original failed attempt remains
in `bootstrap-fcab25d.log`; subsequent attempts also retain bootstrap transcripts
outside disposable checkouts, including failures before result JSON exists.

## Official GPU result

[Immutable run 20260918-170609](https://github.com/Flyingzaptop/ARC/tree/results/stage15-wicked-20260918-170609/results/stage15-wicked/20260918-170609)
uses ARC `b92c5ac` and pinned Wicked
`0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`.

- All graph/backend/observation checks passed; no Present or device-loss failure.
- Scene retrieval, same-scene recall and positive identity margins: 100%.
- Mean identity margin: 0.0819 (required 0.02); two clusters; recurrence 80%.
- Stage 14.5 GPU p50: 3.196928 -> 3.009536 ms (-5.86%); 3/5 scene wins.
- Overall FAIL: adaptive action rate 5/94 = 0.053191 exceeds 0.05, and the final
  semantic snapshot has 122 alive resources versus the required 128.

No threshold was lowered and no failed run was discarded. Stage 15 is **not
frozen**. The resource-population snapshot is taken after recovery has switched
to HelloWorld; it does not describe the measured scenes' live population.
The classification evidence is also limited: baseline captures all joined one
online cluster; adaptive water created the second after centroid drift. The
80% recurrence gate passes, but this should not be described as perfectly
stable workload clustering.

The 65k scene's GPU p50 changed from 5.763 to 15.982 ms while CPU throughput
remained much lower than the GPU-only timings imply. Counterbalanced runs are
needed before attributing that change to ARC or hardware variability.

## Counterbalanced measurement and resulting follow-up

The `b92c5ac` three-arm experiment completed one calibration and three rotated
triples (20 seconds measured per arm). Equal-scene mean ratios were:

| Comparison | Frame-interval p50 ratio | GPU p50 ratio |
| --- | --- | --- |
| Observe / OFF | 1.0586 | 1.1673 |
| Adaptive / OFF | 1.0532 | 1.1528 |

These runs do **not** demonstrate an overall product speedup. GPU ratios have
large scene/run variation, especially 65k instances. Frame-interval timing is
wall-clock cadence, not CPU thread utilization. Raw scenes/order/sample counts
and the executable SHA256 are retained; no favorable run was selected out.

Inspection then found that read-only submissions accumulated pending residency
records until a fence signal, and Wicked injected its own GPU signals even
though it had no controlled residency resources. A 100,000-read-only-submission
regression failed before the fix and passed afterwards. `de1e106` retains fence
bookkeeping only for controlled resources and avoids those extra GPU signals
when the controlled population is zero. This removes unnecessary work without
changing controlled-resource synchronization; performance impact must still be
measured rather than inferred from the code change.

The same follow-up captures resource semantics at the end of measured adaptive
work, before switching to HelloWorld for recovery, and records the capture phase
and frame explicitly. The population threshold remains 128.

## Latest GPU evidence and interruption

[Run 20260918-172800](https://github.com/Flyingzaptop/ARC/tree/results/stage15-wicked-20260918-172800/results/stage15-wicked/20260918-172800)
tested `de1e106` with 60 seconds per baseline/adaptive phase. All Stage 15-specific
gates passed: 129 measured live resources, retrieval/recall/positive margins 100%,
mean identity margin 0.0821552, two clusters, recurrence 80%. Graph/backend were
clean, extra fence signals were zero, quality recovered, and action rate was
0.041237. Overall acceptance remains FAIL: GPU p50 gain 4.6578% versus required
5%, with 1/5 winning scenes versus required 2. The longer duration is recorded;
the earlier 30-second failure remains published and is not replaced.

The subsequent three-arm comparison completed calibration but crashed during
`run-00-off` on 2026-09-18 around 17:34 local time. The user saw Present device
loss `0x887a0005` / `DXGI_ERROR_DEVICE_REMOVED`; crash output contains exception
`0xC0000005`, and the process exited -1073741819. OFF constructs no ARC host and
does not run observation/control, but retains the shared renderer harness and
callback branches. This does not establish a hardware fault or rule out a
renderer/harness problem. Experiment scene/phase breadcrumbs currently retain
default values, so their zero values must not be used to identify the crash scene.

No test processes remain active and no further GPU runs were started. Incomplete
comparison files and failure log were preserved in
the runtime-hardening evidence branch. Resume from the `ARC-hardening` worktree
on `codex_den/runtime-hardening`, without subagents. Do not freeze Stage 15 or
start Stage 16 until the remaining acceptance issue is addressed.

## Remaining boundaries

- Live resource population is owned by the renderer. Queue and command-list
  registries still require destruction events before the entire runtime can be
  described as globally memory-bounded; see [history policy](RUNTIME_HISTORY.md).
- Resource-role accuracy still needs independent labeled evaluation. No
  inferred semantic class grants mutation permission.
- Summed hook durations include overlapping worker time and are not frame
  critical-path latency. Histograms exclude their own recording cost.
- Counterbalancing reduces order bias; it does not prove image equivalence or
  remove all thermal/background noise. See [measurement scope](WICKED_MEASUREMENT.md).

The original checkout and its local Stage 2 report edit were preserved. Work
was performed in a separate worktree and published without rewriting historical
result branches.
