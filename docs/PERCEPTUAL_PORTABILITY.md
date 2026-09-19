# Independent renderer portability

The same `arc-core.lib` implements F/G policy and critic for the native compute
fixture and Microsoft's D3D12HelloTexture raster renderer. The adapter overlay
does not introduce any renderer-name branch into core code.

Upstream Microsoft revision: `213dd4fd4918ea009dd8f35adee1aff1f2ecaba4`.
`patch-perceptual-hello-texture.py` requires a clean isolated checkout at this
revision. It adds an ordinary box-filtered mip to the original checkerboard,
observes GPU timestamps around the frame's rendering commands and reads back the
actual backbuffer. The original shader, geometry and 1280x720 output are retained.
The actuator changes/restores a DX12 SRV, not an engine graphics setting.

Configure the optional `ARC_MICROSOFT_PERCEPTUAL_SAMPLE`, `ARC_AGILITY_NATIVE` and
`ARC_DXC_EXECUTABLE` paths, build `arc-microsoft-perceptual`, then run with
`ARC_PERCEPTUAL_OUTPUT` pointing at a fresh result directory. Assets are compiled
with pinned DXC and the Agility runtime is copied beside the executable.

The independent validator recomputes the raw image and timing result. The observed
case preserved the image exactly but saved only 0.001024 ms (0.01536 -> 0.014336 ms),
below the unchanged 0.02 ms benefit floor. The shared controller rejected retention
and restored the original SRV. This is a correct portable decision, not a speedup
claim. Positive acceptance and harmful-action rejection are separately demonstrated
by the native fixture.

Early negative runs are retained: missing compiled shader assets prevented startup,
then draw-only timestamps were below timer resolution. The build now produces the
upstream `.cso` files and measures the actual frame command interval. Thresholds
were not lowered to accommodate the second renderer.

This verifies portability of the trial protocol and decisions to an independent
raster renderer. It is not validation of generic game observation, universal
visibility, or prediction accuracy in every engine.
