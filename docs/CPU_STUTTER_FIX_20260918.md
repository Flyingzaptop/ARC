# CPU frame-stall root cause and fix

The ARC Wicked harness broadcast `wi::eventhandler::SetVSync(false)` every frame
and on scene switches. In pinned Wicked
`0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`, that is not an idempotent setter:

```
SetVSync(false)
  -> EVENT_SET_VSYNC callback in Application::SetWindow
  -> GraphicsDevice_DX12::CreateSwapChain
  -> WaitForGPU
  -> release back buffers
  -> IDXGISwapChain::ResizeBuffers
```

The callback does this even when VSync is already disabled. The unnecessary GPU
wait and swapchain rebuild ran on the main thread. It also ran in our OFF arm,
which disabled the ARC host but retained the shared harness. Earlier OFF results
therefore did not rule out an ARC integration bug. This is a confirmed harness
defect, not evidence of a broken laptop GPU. Its relationship to earlier device
removal incidents has not been independently established.

## Reproduction and isolation

Windows denied WPR's system profiling policy (`0xc5585011`). No privileges,
driver settings or security policies were changed. Added optional, bounded
in-process diagnostics (`ab83591`) exporting raw engine CPU ranges and coarse
application scopes. Single-scene runs lasted eight seconds, capped at 100,000
CSV rows, with a launcher timeout and process cleanup. Normal benchmark mode
does not write these profiles.

For the simple model, SetVSync itself measured p50 **11.50 ms**, p95 **17.34 ms**,
maximum **173.32 ms**. Application Update measured p50 11.66 ms while actual
RenderPath3D Update was only 0.15 ms. In 65k instances, SetVSync p50 was 26.80 ms.
This narrowed the delay to the repeated presentation event, rather than merely
inferring CPU trouble from GPU/frame-time differences.

## Fix

- Configure `tests.swapChain.desc.vsync = false` before the initial `SetWindow`.
- Remove presentation-event broadcasts from the entire runtime/scene harness.
- Keep native resolution, quality settings, content and all 65,536 instances.
- Add a regression contract rejecting SetVSync broadcasts in the runtime
  bridge, and register it in CTest and official bootstrap validation.

The regression failed on the reproducing code and passed after the fix.

## Local paired measurements

Same laptop and pinned engine; OFF arm, same profiling instrumentation, eight
seconds per process. Summaries exclude the first 4,000 ms and frames <=10.
No device-removal error occurred in these short diagnostic runs.

| Metric, p50 milliseconds | Before | After |
| --- | ---: | ---: |
| Model: engine CPU Frame range | 12.50 | 1.03 |
| Model: frame cadence | 13.13 | 6.94 |
| 65k: engine CPU Frame range | 42.86 | 19.06 |
| 65k: frame cadence | 44.01 | 19.92 |

With ARC observation enabled after the fix, cadence p50 was 6.94 ms (model)
and 19.13 ms (65k). The user independently reported that visible freezes were
gone during the corrected runs. Release and Debug core/deterministic/stress
CTest selections each passed 36/36, including the new presentation regression.

These are short diagnostic comparisons, not product-performance acceptance or
an image-quality claim. Engine `CPU Frame` is inclusive wall time ending before
SubmitCommandLists; it is not CPU utilization or full frame cadence. Cadence is
derived from consecutive Application Update timestamps. After the fix, ordinary
submission/presentation waiting remains (model SubmitCommandLists p50 ~5.9 ms),
so the CPU-range reduction must not be described as a 12x FPS improvement.

Initial scene loading is separate: the first model load in the cold diagnostic
run took ~2 seconds; 65k setup also deliberately duplicates entities. These
startup costs and the scene's remaining animation/culling work were not removed
or hidden by weakening the workload.

## Reproduce

Apply `scripts/apply-wicked-engine-integration.ps1` to a clean pinned checkout
and build Tests. Run `scripts/profile-wicked-cpu.ps1` with explicit Executable,
WorkingDirectory and Output paths; Scene 1 selects model, 18 selects 65k,
Mode is off or observe, and Seconds is bounded to 2..15. Optional AlwaysActive
controls only the diagnostic application's normal background-pause behavior.
Use `scripts/summarize-wicked-cpu.ps1 -Csv <paths>` for raw-range and cadence
summaries. Profiles are diagnostics only and never feed ARC inference/control.

Earlier performance comparisons include repeated swapchain recreation and
must not be treated as reliable estimates of ARC overhead. Stage 14.5/15
performance acceptance must be rerun on this corrected harness; no gate was
relaxed and Stage 15 has not been frozen by this fix.
