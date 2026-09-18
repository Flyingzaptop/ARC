# Stage 14 external D3D12 renderer acceptance

This acceptance moves ARC beyond the repository-owned reference renderer.

## Pinned external workload

- Upstream: `microsoft/DirectX-Graphics-Samples`
- Sample: `Samples/Desktop/D3D12HelloWorld/src/HelloTexture`
- Pinned commit: `213dd4fd4918ea009dd8f35adee1aff1f2ecaba4`
- Upstream license: MIT

ARC does not vendor the upstream sample. The bootstrap clones the exact commit and
applies `scripts/apply-directx-hello-texture-integration.ps1` to the temporary
checkout. The overlay fails closed when an expected source anchor or upstream SHA
does not match.

## What ARC observes

The external renderer reports successful D3D12 operations through
`arc::dx12::NativeHostAdapter`: resource and descriptor lifecycle, command-list
reset/close, barriers, resource use, queue submission, fence signal/completion and
Present.

The renderer keeps ownership of its device, swapchain, command allocator, command
list, PSO and resources. ARC does not hook COM vtables or inject into another
process.

## Controlled quality action

The integration adds one explicit renderer-owned quality actuator: a root constant
selects the number of texture samples used by the external sample pixel shader.
ARC receives a reversible Bandwidth-domain quality profile and changes only this
host-approved actuator.

The baseline always runs at full quality. The adaptive phase uses the same external
render loop and target, with ARC closed-loop control enabled. Recovery uses a
relaxed target and must return the actuator and controller state to full quality.

## Acceptance is intentionally stricter than Mega Stage B

A PASS requires all integration/safety gates plus:

- baseline miss ratio >= 25%, so there is meaningful pressure to solve;
- miss ratio reduction >= 10%;
- p50 frame-time reduction >= 5%;
- adaptive p99 no worse than 10% above baseline;
- at least two physical quality actions and at least one learned effect;
- no more than 24 quality actions and 8 direction changes;
- full quality recovery;
- zero backend, graph, bridge or observation errors;
- native 1920x1080 and temporal/upscaling/frame generation disabled.

Reducing only the average size of misses is not sufficient for this acceptance.

## One-click path

`scripts/stage14-external-bootstrap.ps1` builds ARC, runs deterministic
anti-chatter/host-adapter regressions, restores and builds the pinned Microsoft
sample, runs the external acceptance, writes machine-readable artifacts, and
publishes them to `results/stage14-external-<timestamp>`.

A failed performance acceptance is still published for diagnosis. Build, provenance
or overlay failures stop before a misleading benchmark result can be produced.
