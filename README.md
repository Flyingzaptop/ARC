# ARC — Adaptive Runtime Core

ARC begins as a read-only D3D12 observer. Its first job is to reconstruct GPU
resource lifetime, memory pressure, queue activity, and use history without
changing application output.

## Current implementation

The initial core provides backend-neutral stable IDs, a monotonic clock, a
fixed-capacity SPSC event ring, and a versioned/recoverable binary trace format.
No resource residency or rendering decisions are implemented.

## Build

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`arc-trace` writes and validates a small example trace. `arc-calibrate` is a
placeholder for the Stage-1 hardware profiling executable.
