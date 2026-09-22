# Cauldron target-FPS sweep

Baseline median: **79.021 FPS**. Targets are calculated from this value.

| Mode | Target FPS | Mean FPS | Median FPS | Last 10s FPS | 1% low | Time at/above target | p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| 00-baseline | — | 78.40 | 79.02 | 74.69 | 63.94 | — | 15.06 |
| 01-x1.25 | 98.78 | 90.99 | 92.87 | 99.86 | 54.81 | 29.7% | 16.28 |
| 02-x1.5 | 118.53 | 91.78 | 94.46 | 96.97 | 52.83 | 2.8% | 16.11 |
| 03-x2 | 158.04 | 83.56 | 85.59 | 85.68 | 57.42 | 0.0% | 16.02 |
| 04-x2.5 | 197.55 | 92.36 | 95.92 | 102.01 | 56.81 | 0.0% | 15.50 |
| 05-x5 | 395.11 | 85.81 | 87.02 | 93.58 | 56.92 | 0.0% | 15.64 |

60-second measurement windows after the same 120-frame host warmup. No separate controller initialization is excluded. Each ARC run starts from an identical copied shader-cache seed. Camera route/settings are identical; camera animation is frame-driven, so different FPS traverse different numbers of cycles. One run per goal; not a repeated causal benchmark.

## Hardware and focus

- 00-baseline: power-limit readings [116.5, 126.66, 127.92, 129.41, 130.0] W; temperature [52.0, 72.0] C; graphics clocks [1852.0, 1980.0] MHz; foreground fraction 0.653.
- 01-x1.25: power-limit readings [122.73, 122.93, 123.09, 123.9, 124.59, 125.78, 126.3, 126.44, 126.58, 126.86, 127.0, 127.17, 127.37, 127.79, 127.97, 128.04, 128.26, 128.46, 129.47, 129.67, 129.79, 129.85, 129.88, 129.99, 130.0] W; temperature [70.0, 81.0] C; graphics clocks [1837.0, 1972.0] MHz; foreground fraction 0.130.
- 02-x1.5: power-limit readings [115.0, 116.2, 120.67, 121.55, 122.68, 124.31, 125.83, 126.21, 126.24, 127.84, 128.04, 128.35, 128.39, 128.83, 129.08, 129.12, 129.34, 129.37, 129.38, 129.59, 129.74, 129.95, 130.0] W; temperature [74.0, 85.0] C; graphics clocks [1777.0, 1957.0] MHz; foreground fraction 0.199.
- 03-x2: power-limit readings [115.0, 118.45, 121.67, 121.85, 122.67, 123.27, 124.1, 124.64, 125.25, 127.07, 127.25, 127.45, 127.51, 127.6, 128.01, 128.24, 128.6, 128.74, 128.95, 128.99, 129.06, 129.6, 129.81, 129.88, 129.94, 130.0] W; temperature [81.0, 87.0] C; graphics clocks [1762.0, 1950.0] MHz; foreground fraction 0.389.
- 04-x2.5: power-limit readings [115.0, 119.53, 121.34, 124.03, 124.94, 125.03, 125.9, 126.12, 126.18, 126.59, 126.63, 126.73, 126.86, 127.03, 127.04, 127.18, 127.43, 128.04, 128.16, 128.26, 128.35, 128.79, 128.87, 128.93, 129.16, 129.27, 129.39, 129.41, 129.44, 129.51, 129.59, 129.79, 129.99, 130.0] W; temperature [77.0, 87.0] C; graphics clocks [1747.0, 1950.0] MHz; foreground fraction 0.000.
- 05-x5: power-limit readings [115.0, 116.01, 117.05, 117.55, 119.02, 119.33, 120.56, 121.37, 121.82, 121.89, 121.94, 121.95, 122.23, 122.31, 122.39, 122.51, 122.66, 122.68, 122.83, 122.86, 123.04, 123.27, 123.35, 123.45, 123.58, 123.65, 123.76, 123.89, 124.02, 124.21, 124.22, 124.24, 124.36, 124.44, 124.91, 125.29, 125.82, 126.19, 126.29, 126.64, 127.15, 127.36, 128.62, 129.62, 129.63, 130.0] W; temperature [80.0, 87.0] C; graphics clocks [1762.0, 1942.0] MHz; foreground fraction 0.000.

## GPU pass costs (host measurements, milliseconds)

| Mode | GPU span | Lighting | Shadows | GBuffer | Brixelizer update | GI update |
|---|---:|---:|---:|---:|---:|---:|
| 00-baseline | 12.74 | 3.56 | 3.48 | 1.30 | 1.28 | 2.35 |
| 01-x1.25 | 10.86 | 1.85 | 3.37 | 1.26 | 1.29 | 2.33 |
| 02-x1.5 | 10.82 | 1.54 | 3.47 | 1.30 | 1.32 | 2.38 |
| 03-x2 | 11.92 | 2.43 | 3.57 | 1.33 | 1.33 | 2.45 |
| 04-x2.5 | 10.79 | 1.57 | 3.45 | 1.28 | 1.31 | 2.40 |
| 05-x5 | 11.60 | 1.71 | 3.77 | 1.40 | 1.38 | 2.51 |

Raw frames, per-frame fps.csv, manifests, GPU sensor logs and controller decisions remain in each run folder. No image quality assertion is made.

## Stop and interpretation

User stopped further benchmarking. The x10 attempt ended after 243 measured frames; it is incomplete and excluded from the table. No retry or warm baseline was run. Complete baseline plus x1.25, x1.5, x2, x2.5 and x5 results are retained.

The current feedback gate reacts to a binary budget miss, not deficit magnitude. It permits a single mutation after two observations spaced about two seconds, followed by evaluation. Very high targets therefore do not produce proportionally faster adaptation. Sequential timing can confuse scene changes with the effect of a mutation. The main supported lighting pass gets cheaper, but large unmodified raster and GI costs remain. Higher requested FPS cannot remove those costs with the present actuators.
