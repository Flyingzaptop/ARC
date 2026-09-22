# Shadow pass predication and upload snapshots

Intermediate checkpoint; live generic hook integration is not enabled.

Microsoft documents ClearDepthStencilView as predicable, along with draw calls:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication
Earlier reports overstated the need to rebuild a complete command stream. A single GPU predicate can guard the original clear and draw calls. Unknown state still requires original execution.

Added GpuControl::shadow_reuse_predicate using the reserved 64-bit slot, with NOT_EQUAL_ZERO (nonzero skips). Existing bounded upload ring, queue ordering and neutral GPU epilogue are retained. Missing updates default to original execution. Native test now always records both clear and draw; it no longer implements reuse by omitting CPU recording. Occlusion queries prove five GPU-skipped draws across eight frames; nonblank depth texture comparison with an independently rendered oracle proves clear is also skipped. Movement restores rendering. This is correctness/avoided-work evidence, not a GPU-time speedup claim.

Added snapshot_shadow_uploads: current bytes, input boundaries, 1 MiB/64-input limit, upload heap only. Persistent mapped CPU writes are detected with unchanged resource identity. Out-of-bounds ranges, missing data and GPU/default-heap resources are rejected. This helper does not prove complete shader dependencies and is not yet invoked by the live submission adapter.

Release probe/worker-related build and pixel/compute/ray native regressions pass. The runtime still needs pass boundary/state interception, descriptor/resource lifetime ownership, complete dependency admission and submission-time snapshot wiring. No game-level shadow reuse claim is made.
