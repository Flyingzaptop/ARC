# ARC Stage 5 — Steam Launcher and Real-Game Telemetry

## What works now

`arc-launcher.exe` scans installed Steam libraries from `libraryfolders.vdf` and `appmanifest_*.acf`, shows installed titles, launches the selected title through the normal Steam URI, and starts a capture session.

The capture stack is external and non-invasive:

- Intel PresentMon Console: FPS/frame-time capture across DirectX/OpenGL/Vulkan.
- `arc-game-monitor.exe`: DXGI local video-memory budget/usage sampling.
- `game-capture-summary.ps1`: average FPS, approximate 1% low, P95/P99 frame time and stutter count.

Captures are written under `%LOCALAPPDATA%\ARC\captures`.

## User and Advanced modes

The default user flow is: select installed Steam game -> **Play through ARC**.

Advanced exposes:

- `Baseline`: PresentMon capture only.
- `ARC Observe`: PresentMon plus ARC's external DXGI monitor.
- capture duration.
- PresentMon install action.

For fair A/B testing, use the same game scene/settings, capture duration and thermal/power conditions. Run at least three baseline/observe pairs and alternate order when practical.

## Important boundary

Stage 5 external observe is **not** the native resource-control adapter. It does not inject a DLL, replace D3D12, bypass anti-cheat, or mutate a commercial game's resources.

Therefore a Bodycam `ARC Observe` run is expected to be approximately performance-neutral. It validates the launcher, capture path, Steam flow and external telemetry overhead. It is not yet a test of ARC's Evict/Demote policy inside Bodycam.

The Stage 4 controlled D3D12 runtime remains proven in ARC-owned labs. Moving that control into a native game requires an explicit supported integration/interception adapter and a safe resource classification path.

## Bodycam

Bodycam is Steam app `2406770`, is an Unreal Engine 5 / DirectX 12 workload, and is suitable as a demanding external telemetry benchmark. Because it is multiplayer and its developers are working on anti-cheat integration, ARC Stage 5 must remain external/non-invasive for this title.

## What to compare

Primary metrics:

1. average FPS;
2. 1% low FPS;
3. P95/P99 frame time;
4. stutter count / percent;
5. DXGI local memory usage and budget pressure.

When native Controlled integration exists, success is **not only more average FPS**. ARC can also win by maintaining the same FPS/quality with lower resident VRAM, reducing severe frame-time spikes under memory pressure, or allowing higher texture/render quality within the same VRAM budget.
