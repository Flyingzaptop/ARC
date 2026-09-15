# 08 — Repository and build plan

## Suggested repository

```text
arc/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── cmake/
├── include/arc/
├── src/
│   ├── core/
│   │   ├── ids/
│   │   ├── events/
│   │   ├── resource_graph/
│   │   ├── telemetry/
│   │   ├── budgets/
│   │   ├── classifier/
│   │   ├── predictor/
│   │   ├── governor/
│   │   └── safety/
│   ├── backends/
│   │   ├── dx12/
│   │   ├── vulkan/
│   │   └── null/
│   ├── calibrate/
│   ├── launcher/
│   └── tools/
│       ├── trace_viewer/
│       └── resource_dump/
├── samples/
│   ├── dx12_memory_pressure/
│   ├── dx12_texture_pressure/
│   ├── vk_memory_pressure/
│   └── streaming_city/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── regression/
├── research/
│   ├── ps5/
│   ├── framegen/
│   └── driver/
└── traces/
```

## Language

Recommended:

- C++23 runtime;
- CMake + Ninja;
- MSVC initially on Windows;
- Clang-cl optional;
- Python only for offline analysis/plots/ML.

## First build targets

```text
arc-core
arc-calibrate
arc-trace
arc-resource-graph
arc-dx12-observer
dx12-memory-pressure
arc-trace-viewer
```

Vulkan begins only after the D3D12 event/resource model stabilizes.

## First commits

1. repository skeleton;
2. coding conventions and CI;
3. stable ID allocator;
4. high-resolution monotonic clock abstraction;
5. binary event schema;
6. lock-free/thread-local event buffer;
7. hardware discovery;
8. DXGI memory-budget reader;
9. calibration harness;
10. first D3D12 sample;
11. D3D12 resource observation;
12. resource graph;
13. trace writer/parser;
14. ground-truth validator.

Do not start with UI polish.
