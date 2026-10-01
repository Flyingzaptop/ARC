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

## ARC 2.0 implementation

`Game → ARC D3D12 Frontend → ARC IR → guarded rewrite → native D3D12`

The experimental frontend now owns application-facing COM wrappers and captures
state before forwarding. It includes resource/descriptor identities, command-state
snapshots, root binding and shader identities, symbolic access intervals,
submissions/fences, DXGI Present/resize, and a bridge to the existing ResourceGraph
and read-only resource/scene inference. A narrowly admitted exact duplicate-clear
rewrite passes owned GPU readback, critic and rollback tests.

The delivered source revision records typed Draw/DrawIndexed counts and starts,
Dispatch group counts, and ExecuteIndirect object IDs and offsets before native
forwarding. It also records vertex stride, index format and full scissor arrays.
Owned native compute assertions and targeted runs on all five codebases validate
their captured shape. The published performance matrices used an earlier DLL;
they do not establish performance or full image acceptance of the delivered
revision. Command payloads express API requests, not completed GPU effects.

This does **not** mean the entire legacy optimizer has migrated. General shader
transforms, temporal visibility, visual importance and memory actuation remain
separate migration work. Symbolic or unknown dependencies are not treated as
permission to rewrite. Native-supported interface gaps are reported explicitly.

The same frontend has rendered owned workloads from **Wicked Engine, AMD
Cauldron/FidelityFX, Microsoft MiniEngine, Diligent Engine and bgfx**. Startup is
not performance or quality acceptance. Initial measured results are negative;
a history-retirement regression was fixed and the revised matrix is documented
separately. No independent-engine net acceleration or universal compatibility is
claimed. See [architecture](docs/ARC2_ARCHITECTURE.md), [IR](docs/ARC2_IR.md),
[migration limits](docs/ARC2_MIGRATION.md), [test matrix](docs/ARC2_TEST_MATRIX.md)
and [measurements](docs/ARC2_RESULTS.md). The [validation coverage map](docs/ARC2_VALIDATION_LIMITS.md)
distinguishes unit evidence, native GPU evidence and still-unsupported cases.

Testbeds redirect only D3D12/DXGI creation through the generic bootstrap; their
normal graphics calls remain standard COM APIs. Deterministic timing, scene
selection and GPU readback patches are test-harness instrumentation, not engine
semantic hints. Deployment into arbitrary unmodified commercial games is not
established by these source-bootstrap experiments.

## Build and test

Windows requires Visual Studio C++ tools, a Windows SDK and CMake 3.24+:

```powershell
cmake -S . -B build -DARC_BUILD_TESTS=ON -DARC2_FRONTEND=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

GPU-backed tests are opt-in with `-DARC_GPU_TESTS=ON`; they require a compatible GPU.
Use `-DARC2_CPU_DIAGNOSTICS=ON` to additionally build the separate
`arc2-frontend-meter.dll`; its measurement overhead is not part of the normal DLL.
Historical controlled-memory validation remains `run-stage2-final.cmd`. Python
research tests require Python and the dependencies documented with each experiment.
Do not treat unavailable GPU tests or unsupported interfaces as passing evidence.

## Canonical history

`master` is the GitHub default and contains the former canonical
`codex_den/cpu-gpu-offload` history through `d636217`. The pre-migration default is
preserved by `freeze/pre-arc2-master-20260930`. ARC2 development proceeds on
`arc2/runtime-ir`, with small tested commits and milestone pushes. Historical
milestone/result branches and CPU research are retained; no force-push is needed.
