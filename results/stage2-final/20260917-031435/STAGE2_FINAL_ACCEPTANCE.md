# ARC Stage 2 Final Acceptance

- Verdict: **ACCEPTED_WITH_PERFORMANCE_WARNINGS**
- Commit: $commit
- Timestamp UTC: 2026-09-17T03:14:35.4672990Z
- Working tree dirty: False

## Correctness gates

- Tiled mip content integrity: True
- Global memory content integrity: True
- Global action mix (Evict + Demote + MakeResident + Promote): True
- DXGI physical-relief evidence: True ($(@{tiled_resources_tier=3; synthetic_budget_bytes=536870912; synthetic_usage_bytes=524288000; requested_relief_bytes=121634816; planned_relief_bytes=134217728; expected_physical_relief_bytes=134217728; eviction_actions=1; demotion_actions=1; restore_headroom_bytes=134217728; planned_restore_bytes=134217728; make_resident_actions=1; promotion_actions=1; dxgi_usage_before_relief_bytes=169619456; dxgi_usage_after_relief_min_bytes=35401728; dxgi_observed_relief_bytes=134217728; dxgi_usage_after_restore_bytes=169619456; initial_buffer_verified=True; initial_texture_verified=True; lower_mip_preserved=True; restored_buffer_verified=True; restored_texture_verified=True; debug_error_count=0}.dxgi_observed_relief_bytes) bytes observed)
- D3D12 debug layer available: True
- D3D12 debug layer clean: True

## Performance gates

- Light observer P99 delta: -0.4466 ms (target <= 0.15 ms): True
- Light observer CPU delta: 36.752 % (target <= 2 %): False
- Safe frontier point found: True
- Frontier recommendation: managed=45% resident_fraction=0.341 worst_p99_delta_ms=0.0199

## Global memory lab

- Requested relief: 121634816 bytes
- Planned relief: 134217728 bytes
- DXGI observed relief: 134217728 bytes
- Eviction actions: 1
- Texture demotions: 1
- Make-resident actions: 1
- Texture promotions: 1

## Artifacts

- 	races/stage2-final-acceptance.json
- 	races/benchmark-summary.json
- 	races/residency-benchmark-summary.json
- 	races/residency-frontier-summary.json
- 	races/residency-frontier.csv
- 	races/tiled-texture-lab.json
- 	races/global-memory-lab.json
- 	races/hardware-profile.json
