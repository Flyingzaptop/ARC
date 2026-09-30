# ARC — Adaptive Runtime Core

ARC is an experimental graphics runtime and optimizer. It observes graphics work,
models resources and dependencies, and tests reversible quality/performance actions.
It is **not a universal 2x FPS optimizer**.

## Implemented and measured

The existing ARC Legacy / Observer Backend includes D3D12 interception, a resource
graph, descriptor and lifetime ledgers, GPU attribution, scene/visibility/importance
analysis, reversible GPU actions, image critics, rollback, and memory arbitration.
An experimental CPU backend investigates machine-code tasks and CPU-to-GPU offload.
These subsystems and their evidence are retained.

A manually integrated Wicked indirect experiment reduced Present-return interval
by about 1.49 ms (+6.8% Present Hz in its short series); it is not automatic universal
offload or a display-FPS measurement. Generated large-loop GPU experiments were
negative after CPU gathering and transfer costs. See the
[latest composite-loop report](docs/reports/2026-09-27-composite-offload.md).
Older Stage 1/2 documents describe historical controlled milestones, not the whole
current product.

## ARC 2.0 direction and implementation status

`Game → ARC D3D12 Frontend → ARC IR → Optimizer → native D3D12`

The new path will capture semantic state at application-facing COM calls before
forwarding, rather than reconstructing all state afterward. The native runtime and
driver remain responsible for execution. Unknown accesses remain explicit and
prevent unsafe changes. This baseline commit establishes the migration; it does
not claim that the frontend or five-engine validation is already implemented.
See [ARC2 architecture](docs/ARC2_ARCHITECTURE.md).

Validation targets: **Wicked Engine, AMD Cauldron/FidelityFX, Microsoft MiniEngine,
Diligent Engine, and bgfx**, each using D3D12. Availability, coverage, performance
and quality must be reported separately for every workload.

## Build and test

Windows requires Visual Studio C++ tools, a Windows SDK and CMake 3.24+:

```powershell
cmake -S . -B build -DARC_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

GPU-backed tests are opt-in with `-DARC_GPU_TESTS=ON`; they require a compatible GPU.
Historical controlled-memory validation remains `run-stage2-final.cmd`. Python
research tests require Python and the dependencies documented with each experiment.
Do not treat unavailable GPU tests or unsupported interfaces as passing evidence.

## Canonical history

`master` is the GitHub default and contains the former canonical
`codex_den/cpu-gpu-offload` history through `d636217`. The pre-migration default is
preserved by `freeze/pre-arc2-master-20260930`. ARC2 development proceeds on
`arc2/runtime-ir`, with small tested commits and milestone pushes. Historical
milestone/result branches and CPU research are retained; no force-push is needed.
