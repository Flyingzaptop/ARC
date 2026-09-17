# ARC Stage 7 Mixed Graphics Benchmark

- Source: $sourceSha
- Verdict: **PASS**
- Adapter: NVIDIA GeForce RTX 3060 Laptop GPU
- Native target: **1920x1080**
- Temporal / DLSS / FSR / Frame Generation: **OFF**
- Duration: 60 s + per-domain micro-probes

## Gates
- JSON valid: True
- Mixed graphics path: True
- Native 1080p: True
- Temporal disabled: True
- Cross-domain measured probes valid: True
- Actions selected: True
- Physical GPU P50 improved by >=3%: True
- Physical GPU P99 not regressed by >0.15 ms: True

Selected domains are reported as a metric, not forced as a gate. ARC must stop once the measured frame deficit is closed; forcing an unnecessary second quality degradation would violate the optimizer's purpose.

This benchmark uses real D3D12 graphics passes for geometry, raster/overdraw, texture sampling, lighting and shadow work. ARC measures each candidate action on the local GPU before planning the adaptive phase.
