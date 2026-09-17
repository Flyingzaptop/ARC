# ARC Stage 6 Adaptive Quality Benchmark

- Source: $sourceSha
- Verdict: **PASS**
- Adapter: NVIDIA GeForce RTX 3060 Laptop GPU
- Temporal / DLSS / FSR / Frame Generation: **OFF**
- Duration: 60 s
- Power scheme: Power Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced)

## Gates
- JSON valid: True
- Temporal disabled: True
- Non-temporal actions selected: True
- No temporal action selected: True
- Physical GPU P50 improved by >=2%: True
- Physical GPU P99 not regressed by >0.10 ms: True

This is a deterministic ARC-owned D3D12 workload. It validates policy selection and physical GPU effect under repeatable conditions; it is not yet a claim about arbitrary-game gains.
