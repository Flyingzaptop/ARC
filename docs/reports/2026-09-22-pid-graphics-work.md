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
