# Stage 2A controlled residency lab

Date: 2026-09-15. Scope: controlled ARC workload only. No residency mutation
is connected to arbitrary applications.

## Implementation

The backend-neutral model defines residency identity, Resident/Evicted/
PendingResident/Pinned/Unknown states, explicit cost and safety, deterministic
pressure states and metrics. Unknown and pinned objects cannot transition to
Evicted. Pressure enters at 85%, emergency at 95%, and recovery requires three
consecutive samples below each lower hysteresis threshold. Speculative
promotions are disabled outside Normal.

The controlled D3D12 backend:

- rejects eviction unless a supplied GPU fence has reached the proven safe value;
- calls Evict only for committed resources selected as ControlledSafe;
- uses ID3D12Device3::EnqueueMakeResident with DENY_OVERBUDGET;
- starts residency ahead of predicted use and inserts a queue wait on the
  residency fence immediately before the dependent GPU command;
- registers and unregisters a DXGI video-memory budget notification event;
- continues polling Budget and CurrentUsage for recorded snapshots.

The lab creates 24 committed 2 MiB objects. Four form the hot set; a known cold
object appears every 200 epochs. Lookahead is eight epochs, covering the cold
insertion without evicting the displaced hot object. One object is pinned and
one is unknown. Those two are never passed to Evict. Contents are initialized
with object-specific bytes and fully compared after every GPU readback.

## Five-round results

All ten runs used the D3D12 debug layer. Each run completed 2,000 known future
uses with identical data, zero graph errors, zero trace loss and zero unexpected
residency failure.

| Mean metric | Baseline | ARC |
|---|---:|---:|
| P50 ms | 0.79398 | 0.75986 |
| P95 ms | 1.18344 | 1.16068 |
| P99 ms | 1.32080 | 1.31880 |
| Median run P99 ms | 1.2959 | 1.3054 |
| DXGI usage after allocation bytes | 58,916,864 | 58,916,864 |
| DXGI usage after policy bytes | 63,320,064 | 25,571,328 |
| Bytes passed to successful Evict calls per run | 0 | 67,108,864 |
| Reloads per run | 0 | 32 |
| False evictions | 0 | 0 |
| Late residency | 0 | 0 |

The ARC mean P99 is 0.002 ms lower; the median run P99 is 0.0095 ms higher.
This is treated as stable within run-to-run variation, not an improvement claim.
DXGI CurrentUsage after policy is 37,748,736 bytes (59.6%) below baseline at
that observation point. Bytes evicted exceeds the instantaneous reduction
because some objects are evicted again after rare cold use.

Every run observed at least one budget notification. The lab also deliberately
requests eviction with an incomplete fence value and requires UnsafeInFlight.

## Success criterion

The controlled workload demonstrates materially more controlled CurrentUsage
with bit-identical contents, stable tail timing, zero debug-layer errors, zero
graph errors, zero trace loss, no late residency and no MakeResident failure.
This satisfies Stage 2A for this workload.

## Boundaries

- The mutating backend is linked only to dx12-residency-lab. The existing observer
  samples remain read-only.
- The lab controls committed resources it created. Placed-resource residency
  would operate on the owning heap; reserved tile mappings remain outside scope.
- The future sequence is known. The predictor is deterministic lookahead plus
  ResourceGraph EWMA/recency primitives; it is not a claim of real-game prediction.
- Budget notification delivery is OS-controlled; polling remains authoritative.
- CurrentUsage samples are process/OS estimates and may lag residency operations.
- No texture quality, mips, descriptors, shaders, resolution, sparse mappings,
  frame generation, FSR, ML or third-party process integration was added.

## Reproduction

```powershell
./scripts/validate.ps1 -Configuration Release -GpuTests -DebugLayer -StressTests -Clean
./scripts/residency-benchmark.ps1 -Rounds 5 -Objects 24 -ObjectMiB 2
```

## References

- [D3D12 Evict](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-evict)
- [D3D12 residency flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_residency_flags)
- [DXGI budget notification](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-registervideomemorybudgetchangenotificationevent)

## Stop condition

Stage 2A is complete for the controlled lab. Stage 2B automatic read-only D3D12
interception and Stage 3 adaptive texture residency were not started.

