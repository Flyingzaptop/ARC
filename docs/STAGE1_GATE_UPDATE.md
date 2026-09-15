# Stage 1 gate update — 2026-09-15

## Outcome

The practical Stage 1 gate is passed for the supported explicit D3D12 integration
and controlled workloads. No Stage 2 functionality was added. Unrestricted game
compatibility and unobserved formats/usage patterns are not certified.

## Installation and discovered defect

The user approved UAC installation of `Tools.Graphics.DirectX~~~~0.0.1.0`.
Windows reported `Installed`, installer exit code 0, `RestartNeeded=False`.

Enabling the newly available debug layer reproduced an application failure with
exception/exit code `0x87d` (2173), even in baseline mode. Immediate debug callback
logging exposed `CORRUPTION #921`: final resource release while GPU work was still
in flight. The sample waited for its frame fence before Present, but Present could
enqueue additional queue work before teardown. Waiting on a final graphics fence
after the last Present fixed the source of the defect. Observer behavior was not
relaxed or used to hide the failure.

The long results in the September 14 report predate that fix. Their content checks
and timings were measured, but they did not establish safe teardown without the
debug layer. They must not be cited as having passed debug validation.

## Commits

- `98424c1` — retire Present work before releasing sample resources; immediate
  debug message callback for failures that otherwise terminate without stdout.
- `5479e3e` — enhanced texture barrier GPU test with readback verification; explicit
  skip code for unsupported hardware.
- `9f7e389` — count all callback errors independently of InfoQueue storage capacity,
  unregister callbacks before context destruction; add `-DebugLayer` switch.

## Validation

- Release + D3D12 debug layer: 6/6 CTest tests passed.
- Debug + D3D12 debug layer: 6/6 CTest tests passed.
- Baseline one-iteration reproduction: failed before the fence fix, passed after.
- Mixed Full workload with debug layer: 1,000 iterations passed.
- Continuous corrected mixed Full workload with debug layer: 100,000 iterations,
  33 resources, content checks passed, zero trace drops, zero graph errors and
  complete session. P50 iteration 0.8125 ms, P95 1.4258 ms, P99 1.7139 ms.
  These are debug-validation timings, not production observer overhead.
- Enhanced texture transitions COMMON → COPY_DEST → COPY_SOURCE execute on the
  GPU, preserve uploaded texels on readback and pass debug-layer validation.
- Legal debug warnings remain for ignored buffer initial states, an unused sampler
  comparison function and an unoptimized clear value. They are not suppressed.

## Completion matrix

| Gate criterion | Result |
|---|---|
| Supported committed/placed/reserved/heaps and destruction | PASS |
| Resource/heap/view/copy/queue relationships | PASS |
| Recorded barriers, submissions, fences, presents and budgets | PASS |
| Temporal graph, conservative classification and temperature | PASS |
| Versioned calibration and recoverable traces | PASS |
| Ground truth, output invariance and long-run stability | PASS in controlled scope |
| D3D12 debug validation | PASS after teardown fix |
| Measured/documented observer overhead | PASS; <2% CPU target not established |

## Remaining boundaries

See [STAGE1_STATUS.md](STAGE1_STATUS.md) for supported metadata and interpretation
limits. Enhanced buffer/global/aliasing runtime coverage is not exhaustive;
uncommon formats return UNKNOWN; the integration is explicit rather than an
arbitrary-process interceptor. None of these limitations authorizes modifications
to unknown resources. Stage 2 may be proposed as a separate task, not started here.

## Reproduction

```powershell
./scripts/validate.ps1 -Configuration Release -GpuTests -DebugLayer
./scripts/validate.ps1 -Configuration Debug -GpuTests -DebugLayer
$env:ARC_D3D12_DEBUG = '1'
./build/Release/dx12-mixed-resources.exe full 100000
```
