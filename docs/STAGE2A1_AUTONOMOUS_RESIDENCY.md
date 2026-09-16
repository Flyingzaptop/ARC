# Stage 2A.1 — autonomous residency policy

Status: implementation ready on `dev/stage2a1-autonomous-residency`; backend-neutral policy and simulator are validated in CI. D3D12 `auto` runtime mode requires the dedicated Windows/GPU validation below before this branch is eligible for merge.

## Goal

Remove the Stage 2A oracle from the residency decision path. The autonomous policy must not inspect the workload's future access sequence. It may use only:

- current managed/OS budget state;
- resource state and safety class;
- historical use epochs;
- learned reuse interval;
- resource byte/reload cost;
- last-use queue/fence evidence;
- completed-fence evidence.

The old `arc` lookahead mode remains as an oracle/upper-bound comparison. `baseline` remains the no-residency-policy control.

## Core policy

`ResidencyGovernor` now computes an explicit number of bytes to free from the configured pressure/emergency target instead of returning every cold object. Eviction candidates are limited to the smallest ranked prefix that reaches the requested reduction.

Candidate rules:

- only `ControlledSafe` + `Resident` resources;
- `Pinned` and `Unknown` are never candidates;
- in-flight resources are rejected until their last-use fence is known complete;
- young resources are protected for a minimum age;
- a resource predicted inside the prefetch horizon is protected;
- remaining candidates are ranked by age/reuse history, bytes recovered and reload cost.

Each eviction action carries the queue/fence evidence on which the decision depends. The backend must still validate that fence before calling `Evict`; the action is not permission to bypass backend safety.

## Prediction

Reuse is learned from observed intervals using the existing EWMA. Prediction is periodic and epoch-aware: if an expected cycle has already passed, the predictor advances to the next cycle instead of permanently retaining a stale prediction in the past.

No future workload sequence is provided to the policy.

## Promotion paths

There are deliberately two distinct paths.

### Speculative promotion

`plan_promotions()` is allowed only in `Normal` pressure state. It predicts near-future reuse and may return `MakeResident` candidates up to a cumulative promotion ceiling/headroom limit.

### Demand residency

`require_resident()` is independent of speculative policy. If an evicted controlled-safe object is actually requested, it may be made resident even under Pressure/Emergency because correctness takes priority over memory optimization.

## State machine

Automatic controlled-safe transitions are restricted to:

```text
Resident -> Evicted
Evicted -> PendingResident
PendingResident -> Resident
```

`Pinned` and `Unknown` are immutable through this policy.

## Cross-platform oracle-free simulator

`arc-residency-autonomous-sim-tests` runs a 5,000-epoch hidden access pattern. The governor sees the request only when that epoch arrives; it does not receive the future sequence.

The test includes:

- a four-object hot set;
- medium-period objects;
- rare rotating objects;
- real pressure/recovery transitions in the managed model;
- autonomous eviction;
- speculative promotion;
- demand misses;
- bounded residency checks.

The simulator is a policy regression test, not a claim about real GPU performance. Its purpose is to detect thrashing, broken pressure logic and oracle leakage without requiring D3D12 hardware.

## D3D12 autonomous lab

`dx12-residency-lab` supports three modes:

```text
baseline  no ARC residency mutation
arc       Stage 2A oracle/lookahead reference
auto      Stage 2A.1 autonomous policy
```

`auto` performs a warm-up history phase, then executes a measured phase in which the next resource is revealed only at actual use.

The lab keeps two budget concepts separate:

1. real DXGI Budget/CurrentUsage, recorded as OS telemetry;
2. an ARC-managed sub-budget for the controlled object pool.

The managed sub-budget exists so policy pressure can be tested on ordinary GPUs without allocating several gigabytes simply to approach the OS process budget. It is not reported as the real DXGI budget.

When the autonomous policy selects an eviction, the D3D12 backend performs the real `ID3D12Device::Evict` call using the resource's recorded last-use fence. Promotion uses `ID3D12Device3::EnqueueMakeResident`; the graphics queue waits on the residency fence before dependent GPU use. Every requested resource is copied back and its bytes are compared against its object-specific pattern.

## Metrics

Schema-2 lab output includes:

- `managed_budget`;
- `managed_usage_average`, `managed_usage_peak`, `managed_usage_final`;
- `bytes_evicted`, `bytes_made_resident`;
- `reloads`;
- `late_residency`;
- `speculative_promotions`;
- `false_evictions`;
- real DXGI budget/usage snapshots;
- P50/P95/P99 measured iteration time;
- trace drops and graph errors;
- output-content identity.

A non-zero `late_residency` in `auto` is not a correctness failure by itself: it means the predictor did not bring an object back before demand and the demand path recovered it. It is a quality/performance metric to minimize. Content mismatch, debug-layer error, trace loss or graph error remains a failure.

## Benchmark design

`scripts/residency-benchmark.ps1` rotates execution order across rounds:

```text
baseline -> arc  -> auto
auto     -> baseline -> arc
arc      -> auto -> baseline
```

This reduces systematic thermal/cache/order bias. It writes:

```text
traces/residency-benchmark.json
traces/residency-benchmark-summary.json
```

## Validation already available without the target GPU

- backend-neutral policy compiled and exercised locally with GCC 14 / C++23 and warnings enabled;
- Windows GitHub Actions compiles/runs ordinary policy/core tests in Debug and Release;
- Linux GitHub Actions builds/runs backend-neutral core and residency tests;
- oracle-free simulator is a normal CTest target.

The D3D12 executable is compiled by the Windows CI job even though hosted CI does not opt into GPU execution.

## Required target-machine validation

From the repository root on the Windows/RTX test machine:

```powershell
git fetch origin
git checkout dev/stage2a1-autonomous-residency
./scripts/stage2a1-validate.ps1
```

This performs Release GPU/debug/stress validation, Debug GPU/debug validation, then a rotated 6-round residency benchmark.

Send back:

```text
traces/residency-benchmark.json
traces/residency-benchmark-summary.json
```

If the command fails, preserve the console output and the most recent `traces/residency-lab-*.json` files.

## Acceptance gate

Stage 2A.1 can be accepted for the controlled D3D12 lab when target-hardware runs demonstrate:

- bit-identical readback in all modes;
- zero D3D12 debug-layer errors;
- zero trace drops;
- zero ResourceGraph errors;
- no unsafe in-flight eviction;
- autonomous managed residency materially below the fully resident pool;
- no catastrophic P99 regression;
- late residency measured and stable enough to guide the next predictor iteration.

No arbitrary-game integration, texture-quality changes, mip removal, sparse mappings, FSR/frame generation or ML are enabled by this stage.
