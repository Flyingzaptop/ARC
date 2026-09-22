# PID and graphics expansion — working record

Implemented: frame-time PID (filtered measurement, derivative on measurement,
conditional integral anti-windup, asymmetric slew limits), marginal compute budget
allocation across supported pipelines, asynchronous discovery, joint compute/VRS
control. No engine interface or image critic. Final surface resolution unchanged.

The allocator uses measured pass costs and explicit heuristic marginal cost
priors. These priors are NOT measured causal speedups or quality predictions.

VRS root signatures with UAV or directly indexed resource heaps are excluded.
The native lean-mode test exposed a disabled root-creation hook; preserving this
metadata hook fixed admission. Cached-list, cross-queue, passive rollback and
application-rate preservation pass. A separate UAV-root test confirms rejection.

Tests: target PID and allocation, previous controller, timing windows: 3/3 pass.
Native VRS outputs: pid-vrs-native-02 and pid-vrs-native-uav.
Initial joint dynamic run: pid-compute-vrs-dynamic-01, 60 seconds, 100.05 mean FPS
for target160; not a baseline comparison. Full saturation and restoration observed.
The user explicitly accepts strong degradation as a stress case while actuators
are being expanded. No image-quality certification is claimed.

Remaining: runtime pixel-shader transformations, intermediate resource resizing,
mesh simplification/LOD, independent-ray transformations, temporal reuse/reprojection,
residency control, complete critical-path CPU/GPU attribution. These are NOT
implemented by the PID or by this report. All-game/console support is not claimed.

## Pixel shader transformation — preparation, not live integration

Added `arc-shader-tool pixel-mip:N` (N=0..8 half steps). Supports float32 pixel
Sample, SampleBias and SampleLevel, rejects side-effecting UAV operations and
bindless handles. No root-layout changes in this static variant. DXIL validation
and a native graphics test compare every output pixel to independently compiled
HLSL for +1/+2/+4; neutral and original restoration are exact on the fixture.
An implicit-Sample-only fixture also validates the new SampleBias declaration.

This is NOT enabled in the live game controller. Simply replacing a PSO when
recording would leave cached command lists modified after disabling the policy.
Live integration needs a GPU-controlled neutral path, root-argument preservation,
submission-fenced controls and cached-list/indirect/render-pass tests. No static
PSO swap is advertised as reversible game optimization.

## Dynamic target change validation

`pid-allocation-dynamic-03`: 60-second functional run, target100 ->300 at20.00s
->60 at45.24s. PID saturation and subsequent intensity below0.1 confirmed,
VRS modified submissions observed, final restoration confirmed. Mean97.39FPS
is across changing targets and is not a speedup measurement. The earlier
`pid-allocation-dynamic-02` ran normally at target100 (97.65FPS), but its target
change harness incorrectly read elapsed_ms from status.json instead of JSONL;
no target-change commands were sent. It is not counted as passing that check.
