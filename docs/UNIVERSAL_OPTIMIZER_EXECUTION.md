# Universal adaptive optimizer execution contract

Approved scope: compute/lighting, adaptive VRS, texture sampling, ray work,
shadow/GI reuse and cadence, geometry, CPU overhead, one target-FPS controller,
quality validation and rollback. Nine hours is an estimate, not a termination
deadline. No agents. No commercial-game launches. Individual GPU runs <=60s.

The original runtime at 4c22d8a is observation plus manual uniform VRS. Core
host-provided quality actions do not constitute generic process optimization.
Neither infrastructure nor permanently declining a domain completes this scope.

## Fixed acceptance

- Same DLL on Cauldron and Wicked, without host quality callbacks, source names,
  engine labels or shader-hash allowlists as admission inputs.
- Positive native execution, unsafe-input rejection and rollback per mechanism.
- Full closed loop, changing scenes and targets 1.25x/1.5x/2x baseline.
- Moderate quality, live bounded trials permitted. Initial image limits: SSIM
  >=0.98, normalized mean error <=0.01, tile p99 error <=0.04. Natural baseline
  variation is measured separately, not used to silently relax thresholds.
- CPU steady overhead <=0.2ms/frame mean, GPU <=0.25ms/frame mean on this machine;
  probe costs are also included in end-to-end performance evidence.
- An external heavy-scene FPS gain above baseline variation is required.
  2x is a target and must not be claimed without evidence.
- Native resolution; no DLSS/FSR/frame generation; no forced game CPU affinity.
- Cached-list and in-flight generations remain safe during switch/rollback.
- User game testing remains separate from native/renderer evidence.

## Implementation order and completion ledger

- [ ] Complete resource/root/binding contracts and reversible action boundary.
- [ ] Validated shader analysis/neutral transformation and compute actuator.
- [ ] Spatial VRS and texture sampling actuators.
- [ ] Ray, shadow/GI and geometry actuators.
- [ ] Target FPS, independent quality, adaptation and CPU budgets connected.
- [ ] Two external renderers, regression matrix, net performance and packaging.

Detailed observational code must stay off the normal render path unless needed
by an enabled action. Worker compilation, disk IO and GPU waits never occur on
the render thread. Unknown bindings or dependencies prohibit a mutation.

## Checkpoint 1 (not scope completion)

Root-signature range/register-space/constant-state contracts have CPU tests.
The new shader transformer admits a bounded independent-pixel DXIL compute
class, rejects UAV reads/atomics/nonlocal writes, and retains the original
dispatch footprint while eliminating whole workgroups in coarse mode. A dynamic
CBV selects 1x1/2x1/1x2/2x2. Every output pixel, conditional write and partial edge
passed a real GPU test; one cached native command list switches and rolls back
bit-exactly. The native D3D12 debug layer reports zero errors. All 48 non-GPU
tests pass. The transform is not connected to generic process mutation yet;
physical bindings, alias safety, quality admission and the target loop remain.

Explicit opt-in shader capture exported the heavy renderer's actual bytecode.
Its expensive lighting shader accepts roundtrip, static and controlled variants
under the DXIL validator. Source shader names and benchmark labels were not
transformation inputs. This is not an external performance/quality result.

Evidence (outside source tree):
`C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/`
contains `shader-inventory`, `native-transform-01`, `native-transform-02`, and
the assembled lighting diagnostic variants. Do not publish vendor shader
binaries as ARC-owned redistributable source.

## Checkpoint 2 (experimental process actuator, not scope completion)

The opt-in process interceptor now prepares shader variants in an external
compiler worker, appends a control CBV to a compatible root signature, preserves
application root arguments, and validates actual descriptor/allocation bindings
at each submission. Placed-heap overlap and missing descriptors prevent coarse
execution. GPU command epilogues restore controls to neutral, including cached
replays. The native injected-DLL test (`generic-native-02`) verifies every pixel,
dynamic descriptor-array indexing, all rates and bit-exact cached rollback with
zero D3D12 debug errors. Observer/profiler regressions and 48 non-GPU tests pass.

Explicit diagnostic environment: `ARC_OPTIMIZER_WORKER` (absolute shader-tool
exe), `ARC_OPTIMIZER_COMPILER` (absolute pinned DXC DLL), `ARC_OPTIMIZER_CACHE`
(absolute private cache). `ArcExperimentalCompute` selects neutral/2x1/1x2/2x2/off.
Automatic quality admission remains false; this is not an accepted policy.

First external measurement (`compute-first`): baseline 78.318 FPS, neutral
77.839 FPS, diagnostic 2x1 78.244 FPS. No meaningful gain. Five shader variants
prepare, but the expensive lighting variant is declined on an unknown t12
descriptor: its declared 15-element array is not fully populated. Next work is
generic uniform control-flow/resource-index analysis; never assume an unknown
array entry is unused, and never hardcode this shader's layout or hash.

Descriptor volatility is handled at submission, not just recording; see the
[D3D12 descriptor contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1).
Captured application shader bytecode/IR caches remain local, outside Git.
