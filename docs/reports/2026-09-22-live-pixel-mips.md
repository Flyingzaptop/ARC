# Live pixel-shader mip control

Implemented only the agreed pixel mip actuator. No geometry, RT or engine integration added.

- Float32 Sample/SampleBias/SampleLevel in supported legacy-handle DXIL pixel shaders.
- GPU constant-buffer control, 0..8 half steps (0..+4 mip bias); no final-surface resizing.
- Background shader preparation and graphics PSO creation; original work until ready.
- Preserves observed graphics root arguments, including partially written root constants.
- Control upload at submission and neutral epilogue allow cached-list changes and exact rollback.
- Stop/passive mode neutralizes control; unknown/indirect/bundle state and heap switches decline.
- Bounded 64 captured graphics PSOs, 32 MiB captured shader code, 8 GPU-control objects.
- Does not yet support graphics pipeline-state streams, bindless/modern-handle shaders,
  stream output, or arbitrary shader operations. These run without this transformation.

Validation kept short:
- Native GPU oracle: static and controlled variants, fractional mip steps, invalid control,
  original recovery; live DLL interception, cached-list +1 ->+4 ->off, late unknown state,
  partial root constants and passive-mode rollback: PASS.
- Cauldron 15-second smoke initially prepared variants but declined draws because the root
  completeness check was too strict. After correction, the second 15-second run prepared19
  variants, recorded1048961 modified draws and4119 modified submissions, with zero pixel-module
  faults and confirmed automatic-session restoration. These are activity counters, not FPS gain.
- The final build also includes suspended-render-pass rejection and moves GPU-control allocation
  outside the recording lock. Final native test passed. No long performance matrix was run.

Evidence: C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/pixel-live-cauldron-02
and pixel-live-native-release. The smoke run is not a causal performance comparison.
