# ARC 2.0 semantic frontend

Status: experimental vertical slice under native and independent-codebase validation.
This is not a replacement Windows graphics driver and is not yet production coverage.

## Boundary and actual execution path

```mermaid
flowchart TD
  Game --> Bootstrap[D3D12/DXGI creation bootstrap]
  Bootstrap --> Frontend[ARC COM wrappers]
  Frontend --> IR[ARC IR state at API execution]
  IR --> Guard[Exact structural rewrite guard]
  Guard --> Native[Native D3D12 / DXGI]
  Native --> GPU[Driver / GPU]
  IR --> Snapshot[Bounded immutable analysis snapshot]
  Snapshot --> Graph[Existing ResourceGraph bridge]
  Graph --> Semantics[Existing resource and scene inference]
  Evidence[Independent GPU readbacks] --> Critic[Existing PerceptualCritic + offline verification]
```

The general target remains `Game → ARC Frontend → ARC IR → Optimizer → D3D12`.
The implemented mutation path is deliberately narrow: exact consecutive repeated
RTV clear elimination. The general perceptual action controller is not silently
claimed to be migrated just because its data structures are linked. Temporal
visibility, importance, VRS, approximate shader transforms and dynamic image
admission still require explicit adapters and independent acceptance.

ARC Legacy / Observer Backend and the experimental CPU subsystem are retained.
No engine name, scene name or known shader hash is an optimizer condition.

## Frontend

The bootstrap redirects device/factory creation only. Applications continue to
use standard COM interfaces; dynamic-loading engines can assign the same generic
bootstrap function pointers. Native baseline leaves ARC2_MODE unset. Requested
ARC with a missing DLL fails explicitly, rather than silently measuring native.

D3D12 wrappers own native references and map native IUnknown identity to one live
wrapper. Child APIs unwrap ARC arguments before calling native methods. Descriptor
handles and GPU virtual addresses remain native values with separate IR ledgers.
Unsupported interface versions do not escape as untracked D3D12 objects. This can
make applications requiring those versions unsupported; it is not transparent
compatibility with every native interface. Interface gaps must be measured.

DXGI factory/swapchain wrappers cover factory creation, queue unwrapping,
GetBuffer, identity, Present/Present1, ResizeBuffers and teardown. Adapter/output
interfaces are native; the full DXGI parent/interface closure is not yet claimed.
Factory7 and SwapChain4 availability is currently required by these wrappers.
ResizeBuffers1 with a changed presentation queue has conservative unknown queue
attribution until exact per-buffer queue ownership is established.

## IR and semantics

[ARC2_IR.md](ARC2_IR.md) specifies IDs, generations, bindings, state snapshots,
submissions and synchronization. Resource IDs do not reuse addresses as identity.
Descriptor overwrites advance versions. Work snapshots precede forwarding and
are separate from mutable command-list state. Present attribution is established
only after a successful non-test native Present, with the pre-Present backbuffer.

PSO creation retains SHA256 identities by stage. Available D3D reflection provides
declared register/space/range metadata. This does not prove which dynamically
indexed descriptor executes. Root-signature deserialization connects those ranges
to table bindings. Symbolic may-access intervals remain distinct from exact
resource identities. Reflection failure, unsupported DXIL paths, descriptor
ambiguity and missing modern API state stay unknown.

## Rewriting and validation

A repeated clear is eligible only with the same live RTV descriptor generation,
resource, bit-exact color and ordered rectangle list, and no intervening command
or state change. Reset, overwrite, resource destruction, unknown operations and
unsupported predication revoke eligibility. No cross-frame shadow cache or
approximate sampling is implemented by this rule. The removed command is an exact
redundant overwrite; it is not a quality preset or engine-specific optimization.

The native lab checks reference → modified → reference GPU readbacks, the existing
independent image critic, native debug errors, HRESULTs, COM identity and resize.
Readback/timestamps belong to the harness and are identical across compared modes;
pure frontend passthrough does not inject GPU commands. External-codebase evidence
must be evaluated separately: an owned fixture does not establish broad coverage.

## Cost and modes

`passthrough` captures IR without rewriting or semantic analysis worker.
`observe` adds a bounded-frequency background bridge to ResourceGraph and existing
resource/scene inferencers. `optimize` additionally permits the exact clear rule.
Unknown bindings are not a license to perform approximate rewrites.

Present telemetry uses a bounded memory buffer; disk output occurs at an explicit
session dump while the host is alive, never from a CRT/module destructor. Its wrapper-end timestamp approximates successful
Present-return cadence and never measures physical display FPS. GPU timing is
reported only where the harness actually queries it. The background bridge cost,
IR retention and lock contention are real overhead and are included in mode runs.
Targets are 0.20 ms/frame desirable and <=2% CPU cost diagnostic gate; failure is
reported rather than hidden by selecting a quiet sample.

## Coverage and remaining boundaries

The generated base-interface method inventory is under `src/arc2/generated`.
A forwarded method can still have unsupported semantic coverage; those are
separate dimensions. Modern interface versions, enhanced barriers, DXR state
objects, mesh operations, full shader interpretation and all DXGI interface
escapes require individual verification. See [matrix](ARC2_TEST_MATRIX.md) and
[results](ARC2_RESULTS.md) for actual tested outcomes, not projected support.

Native references: [D3D12 swapchains](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains)
and [core interfaces](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-interfaces).
