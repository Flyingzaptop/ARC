# 04 — Hardware calibration

## 1. Goal

Calibration must answer:

> What can this exact machine sustain today?

Not:

> What usually works on an RTX 3060?

Laptop power limits, thermal state, RAM layout, storage, drivers, and OS conditions vary too much for static presets.

## 2. Measurements

### CPU

Measure:

- core/thread topology;
- single-thread execution proxy;
- multi-thread throughput proxy;
- memory-copy overhead;
- decompression throughput;
- synchronization overhead.

### RAM

Measure:

- total available memory;
- sustained sequential copy bandwidth;
- random-access latency proxy;
- multi-thread copy scaling.

### GPU

Measure:

- dedicated memory size;
- current OS budget;
- graphics workload proxy;
- compute workload proxy;
- copy queue throughput;
- upload throughput;
- readback throughput;
- queue overlap capability;
- shader compilation throughput proxy.

### Storage

For the actual game volume:

- sequential read;
- random read;
- large-block read;
- small-block read;
- latency distribution.

### Display

Capture:

- resolution;
- refresh rate;
- HDR state;
- VRR capability where visible.

## 3. Dynamic budget

Physical VRAM is not the same as safe usable budget.

Maintain:

```text
physical_vram
os_reported_budget
current_usage
ARC_emergency_margin
ARC_safe_ceiling
```

The safe ceiling is dynamic.

## 4. Calibration output

Example schema:

```json
{
  "version": 1,
  "gpu": {
    "vendor": "NVIDIA",
    "device": "example",
    "vram_physical_mb": 6144,
    "vram_safe_mb": 5200,
    "upload_gbps": 11.0,
    "readback_gbps": 8.1
  },
  "ram": {
    "physical_mb": 16384,
    "safe_cache_mb": 7000,
    "copy_gbps": 31.0
  },
  "storage": {
    "seq_read_gbps": 3.0,
    "random_score": 0.71
  }
}
```

## 5. Calibration lifecycle

Use calibration as prior information only.

During games ARC continuously updates:

- actual transfer costs;
- actual thermal behavior;
- actual GPU throughput;
- actual frame-time sensitivity.

Thus:

```text
initial calibration
        ↓
live observations
        ↓
corrected hardware model
```
