# Engine-independent cost attribution

The next dependency after removing duplicate CPU recording is an ARC-owned GPU
cost profile. The existing Cauldron module labels are independent validation
data, not an optimizer input. No engine names, PIX labels, source shader names,
quality settings, FSR or frame generation are used to collect the profile.

Acceptance requirements:

- Capture direct/compute command recordings only after an explicit bounded
  request; terminate after the requested Present windows or a deadline.
- Group graphics work without one timestamp pair per draw; split compute work
  by actual bound compute PSO. Hash shader bytecode and reflect declared bindings
  and thread-group dimensions where available. Missing metadata remains unknown.
- Insert only timestamp queries and their resolution into ARC-owned readback
  storage. Do not replay rendering, alter application shader/state/output, wait
  for GPU work on the render thread, or copy images in the profiling window.
- Resolve only outside render passes and only queries that were ended. Decline
  suspended/resuming render passes and protected-resource tails.
- Retain heaps/readbacks across native object destruction and Reset in flight.
  Reject an overwritten cached-replay sample; do not attribute it to an earlier
  submission. CPU collection and subsequent submission use the same brief lock.
- Bound live recordings (256), intervals per recording (128), pending jobs (512),
  queues (16), pipelines (16384), collected intervals (16384) and active time (10s).
  A failed fence/capacity shortage must retain possibly referenced GPU storage
  until safe, and mark the report partial instead of manufacturing timings.
- Test known compute outputs and native invalid calls; use D3D12 validation.
  Compare ARC's aggregate profile against the heavy renderer's independent
  instrumentation, and explicitly report profiler overhead and coverage gaps.

Implementation flow: successful Reset/Create -> bounded recording -> timestamp
at first work -> close interval at compute PSO/category change -> final timestamp
and ResolveQueryData before Close -> native Execute once -> queue fence -> logger
polls completion -> validate submission serial and queue frequency -> write JSON.
Metadata never authorizes mutation: a declared UAV binding does not prove actual
resource access, temporal stability, valid replacement or safe reduced Dispatch.

GPU timestamp intervals include intervening barriers/stalls. They are not shader
instruction-cycle counters. Queue timelines are kept separate; summing concurrent
queues is not frame time. Time conversion uses floating-point queue frequency.
See [DirectX timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
and [render-pass restrictions](https://microsoft.github.io/DirectX-Specs/d3d/RenderPasses.html).

Cached recordings retain their inserted timestamp/resolve commands until the
application resets or releases them; stopping capture cannot erase immutable
native command buffers. Their storage remains fence-protected even after report
publication. The normal freshly recorded frame path retires this overhead after
the window. Passive hook removal and VRS activation are refused while a profile
is active or has an open recording. Queue references are retired after jobs and
native recording references are gone. Export I/O runs outside the render-call
lock; publication errors terminate that export instead of retrying indefinitely.

Indirect signatures distinguish raster and compute work without reading argument
buffers. Timing remains valid, but actual GPU-generated dimensions are unknown;
`indirect_max_commands` is an upper bound, not a count of executed thread groups.
`frame` is the observed Present epoch at submission, not recording time. Bindless
resource arrays and direct descriptor-heap indexing are reported as metadata gaps.
GPU queries/resolution are not predicated ([DirectX contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication)), so a skipped draw
can produce a valid small timing rather than an uninitialized timestamp pair.

Linked multi-node devices are declined before recording any query commands;
node visibility is not established by this profiler. Raster depth classification
requires both zero color targets and enabled depth/stencil state. UAV-only
graphics without those targets remains unclassified.
