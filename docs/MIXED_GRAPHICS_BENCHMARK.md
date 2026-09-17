# ARC Stage 7 — Mixed Graphics Benchmark

Stage 7 moves ARC validation from a single synthetic compute workload to a repeatable D3D12 graphics workload with independently timed native rendering domains.

## What is measured

The benchmark uses a DIRECT D3D12 queue and separate timestamp regions for:

- shadow-map style geometry work;
- main geometry work;
- raster / overdraw work;
- texture and bandwidth work;
- lighting pixel work.

Each phase reports P50/P95/P99 total GPU work plus P50 GPU time for each domain and local DXGI memory usage/budget.

## ARC decision path

1. Run the baseline mixed scene.
2. Convert measured pass times into `RenderPassTimings`.
3. Build a `FrameBudgetSample` from the measured native workload.
4. Build non-temporal quality candidates for texture, bandwidth, geometry, raster, lighting and shadow.
5. Let the global `AdaptiveQualityOptimizer` select the cheapest visual sacrifice for the measured deficit.
6. Physically change the corresponding D3D12 workload knobs.
7. Run the same mixed workload again and measure the actual effect.

Temporal/upscaling/frame-generation assistance remains disabled by default and is an explicit opt-in path outside Stage 7 acceptance.

## Acceptance

The Stage 7 runner requires all of the following:

- valid benchmark JSON;
- temporal assistance disabled and unused;
- at least one native quality action selected;
- no temporal action selected;
- total physical GPU P50 improves by at least 2%;
- total P99 does not regress by more than 0.15 ms;
- all five real graphics timing domains are present;
- at least one selected action produces a measured improvement in its corresponding physical domain.

A PASS proves ARC can arbitrate between real native graphics workloads under a controlled, repeatable D3D12 environment. It does **not** yet prove arbitrary closed-game interception or gains.

## User workflow

Run `scripts/stage7-bootstrap.ps1`. The bootstrap builds the package, runs CPU policy tests, performs a six-second GPU smoke test, then opens the persistent Stage 7 UI.

The UI keeps the final verdict and metrics visible after the test. The PowerShell runner also stays open until Enter is pressed. Successful or failed runs are published to a `results/stage7-*` branch, and the UI exposes a `Copy Results URL` button.
