# ARC 2.0 architecture

Status: repository baseline; implementation and validation pending.

Game → application-facing D3D12/DXGI COM frontend → ARC IR v0 → dependency graph
→ existing ARC semantic/perceptual core → admitted reversible rewrite → native D3D12.

The frontend owns wrapper identity and application state, not the GPU protocol.
It must snapshot state before forwarding, preserve HRESULTs and native ordering,
track resource/descriptor generations, and express symbolic or unknown accesses.
The legacy observer and experimental CPU backend remain independent control paths.
Passthrough correctness precedes all optimization. A successful interface skeleton
is not acceptance: native readbacks, lifetime tests, overhead and independent
five-codebase coverage are required. This document is expanded on the ARC2 branch.
