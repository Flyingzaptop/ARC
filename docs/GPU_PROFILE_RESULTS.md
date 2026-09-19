# Generic GPU cost profiling: 2026-09-19

ARC can now discover expensive DX12 work without render-module names or engine
callbacks. This change adds profiling and validation, not an FPS optimization.
VRS, shader replacement, upscaling and frame generation are off in these runs.

## Interface

```text
arc-dx12-probe-launch --gpu-profile PID DLL NEW_JSON [PRESENT_WINDOWS]
arc-dx12-probe-launch --gpu-profile-stop PID DLL
```

The DLL must already be initialized in the authorized target. Windows range from
1 to 32; the default is 8. A ten-second deadline ends collection even without
Present. The logger drains completed work asynchronously and publishes a new file.
Do not interpret a timeout, pending jobs, capacity decline or unknown metadata as
complete coverage. Shader metadata is captured on PSO creation; late attachment
and pipeline-library loads can leave metadata unknown.

For the owned benchmark:

```powershell
python scripts/prepare-cauldron-benchmark.py SDK_PATH
# Build FFX_BRIXELIZER_GI, ReleaseDX12, after preparing the adapter.
python scripts/run-cauldron-benchmark.py SDK_PATH FRESH_DIR --dll ABS_PROBE_DLL --mode profile
```

`--mode observe` loads the lean observer without a GPU capture or VRS. `--mode
profile` requests 16 Present epochs after warmup. The historical default with a
DLL remains the explicitly experimental `--mode vrs`.

Graphics work is grouped by output category; compute work is separated by PSO.
The profile contains SHA-256 fingerprints, reflected thread-group sizes and
declared resource bindings. Known indirect signatures retain their compute/raster
classification, but GPU-generated dimensions and executed counts remain unknown.
Graphics with no color targets is classified as depth/stencil only when that
state is enabled. Unclassified work is not silently assigned to an effect.

The profiler records timestamps and resolves them to its own small buffers. It
does not record another rendering command list or copy the frame image. Cached
replay samples that have been overwritten are discarded. Native generations and
GPU fences protect storage across Reset, object destruction and report export.
Stopping capture cannot erase timestamps already recorded in cached lists; those
remain until the application resets/releases the lists. Passive hook removal and
VRS activation are refused while a capture/open recording needs those hooks.

See [design and limits](GPU_PROFILE_DESIGN.md). Linked multi-node devices are
declined; single-node direct and compute queues are supported. Resource binding
declarations are not proof of actual accesses or safe work reduction.

## Independent validation

The external host writes its own GPU marker intervals only to the test report.
`analyze-gpu-profile.py` aligns ARC and host intervals by GPU time. Labels are
used only by this external validator; neither the probe nor the optimizer receives
them. The current host comparison uses one graphics queue at 1 GHz timestamp
frequency. ARC itself converts timings using the actual queue frequency and does
not sum concurrent queue timelines into a frame time.

Two complete captures each produced:

- 16 observed Present epochs, 208 recordings and 128 submitted work recordings.
- 1,088 timed intervals, including 224 indirect API calls.
- 47 distinct compute shader fingerprints.
- Zero faults, capacity declines, missing GPU jobs, unsupported segments or
  overwritten samples in this renderer.
- 98.91% and 99.20% coverage of the independently measured GPU frame interval.
  This is time coverage in this scene, not universal API/resource coverage.

The dominant compute shader has fingerprint
`1635f15fb9c56f3f8ea97264138e477e4f5531273729bd5d435d2c71f27d43ee`.
It dispatches 240x135x1 groups of 8x8x1 threads: a 1920x1080 invocation grid.
Reflection identifies 3 constant buffers, 3 samplers, 22 SRV slots and 2 UAV slots
in 16 binding declarations. It reports no unbounded binding arrays for this
shader. These are declarations, not verified physical input/output resources.

In the primary final capture this shader costs 3.958 ms per Present epoch. Its
GPU interval matches the independent `Deferred Lighting` marker with 99.983%
intersection-over-union. The same fingerprint dominates the repeated capture.
Raster depth/stencil groups cost 4.080 ms; all compute groups total 9.188 ms;
color raster groups total 1.678 ms. These are measured GPU timeline intervals,
including intervening barriers/stalls, not machine-instruction cycle counts.

An earlier exploratory capture found the same dominant shader at approximately
4.4 ms. Timing varies with workload/power state; its identity and independent
interval correspondence remain stable.

## Cost of observing

All runs use the same native 1920x1080 host, 120 warmup frames, 600 measured camera
poses and no limiter. Processes finished in about 15–16 seconds.

| Mode | FPS | 1% low FPS | CPU module preparation, mean ms |
|---|---:|---:|---:|
| No ARC A | 68.31 | 50.89 | 5.367 |
| Lean observation | 68.58 | 52.83 | 5.360 |
| Observation + 16-epoch profile A | 68.25 | 47.57 | 5.898 |
| Observation + 16-epoch profile B | 68.03 | 48.68 | 5.628 |
| No ARC B | 67.04 | 51.46 | 5.463 |

No gain is claimed from these small FPS differences. Profiling has a real burst
cost: first-16-frame CPU module preparation is 7.338/6.955 ms versus 5.013 ms in
the observation run. First-16 frame cadence is 15.271/15.439 ms versus 14.830 ms.
This is a sparse diagnostic, not a reason to enable full profiling every frame.
The probe retires its recordings/buffers after this renderer resets/releases them;
final snapshots report zero retained GPU recordings and zero capture faults.

The timestamp payload for 1,088 intervals is 17,408 bytes. Shader reflection is
CPU metadata; full images are saved separately by the test host after measurement.
Native tests prove exact deterministic compute outputs and raster pixels. The
heavy scene has temporal GI variation: profile-vs-baseline mean linear error is
0.000301, while baseline repeat is 0.000293. No bitwise image identity is claimed
for this stochastic renderer.

The five-run comparison binary is archived separately from the release binary.
The only subsequent runtime change declines linked multi-node devices before
query allocation. The release binary passed the native suite and another complete
heavy capture: 1,088 intervals, zero faults/gaps and the same dominant fingerprint
at 4.132 ms. Manifests preserve both exact DLL hashes.

## What this means for optimization

Even hypothetically eliminating the largest compute interval completely would
only give about 1.36x over the captured GPU interval. A 2x target requires saving
about 7.47 ms here. These are upper-bound/budget calculations, not achievable
speedup claims. Significant work must be removed from more than one expensive
part of rendering, with independent quality validation.

The next implementation dependency is resolving the dominant shader's actual
CBV/SRV/UAV bindings and output contract through DX12, then proving which work can
be replaced. Reflection alone does not prove per-pixel independence, lack of
aliasing or safe dispatch reduction. The generic controller still abstains from
those mutations. No game tests were launched.

Validation completed: 47 non-GPU CTests; native compute profiling on direct and
compute queues; known indirect dispatch; cached replay and overwritten-sample
rejection; Reset while previous work is gated; post-export resource lifetime;
ordinary and suspended/resuming render passes; existing VRS rollback and resource,
readback/lifetime/invalid-call regression tests. Native fixtures use the D3D12
debug layer and verify application outputs.
