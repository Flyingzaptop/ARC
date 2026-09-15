# 12 — Decision log

## D001 — Core is backend-neutral

Reason:
ARC must eventually serve emulator, D3D12, Vulkan, and possibly driver-assisted paths.

## D002 — Observer before optimizer

Reason:
Optimization based on incorrect resource knowledge is worse than no optimization.

## D003 — UNKNOWN resources are immutable

Reason:
Universal resource semantics cannot be inferred safely from dimensions alone.

## D004 — No custom Windows GPU driver in v0.x critical path

Reason:
The effort is too large and unnecessary to prove the ARC concept.

## D005 — Textures are first deliberate quality trade

Reason:
They often dominate VRAM and support graceful mip-based degradation.

## D006 — RL is deferred

Reason:
Safety and explainability matter more than early sophistication.

## D007 — 100% utilization is not itself a target

Reason:
Emergency headroom is necessary to prevent spikes and paging.

## D008 — Temporal features are governed resources

Reason:
Frame generation/upscaling consume memory, compute, bandwidth and latency.

## D009 — DRM/anti-cheat bypass is out of scope

Reason:
ARC should remain a graphics optimization runtime, not an evasion platform.
