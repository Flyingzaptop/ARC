# Runtime graph history

`ResourceGraph` defaults to full history for offline trace analysis. The D3D12
host adapter selects `ResourceGraphRetention::live()`, which retains at most
65,536 submissions, 65,536 copies, and 16,384 destroyed resources. Live resources
are never evicted. Custom nonzero limits can be supplied to the constructor;
zero means unlimited for the corresponding history.

Submission and copy histories use deques, so eviction does not shift the
remaining history. Cumulative workload counters preserve exact checkpoint
deltas after detailed records are removed. Resource tombstone eviction removes
associated views through a reverse descriptor index, including correct handling
of overwritten descriptor slots. Live mode also removes destroyed heaps and
descriptor-heap locations.

Scene checkpoints retain resource usage counters, including retained destroyed
resources. A resource used and destroyed during an observation window therefore
still contributes, and a subsequent checkpoint does not count that usage again.
If resource tombstones are evicted after a checkpoint, its signature becomes
`history_complete=false`. This conservative rule can also reject a checkpoint
when the evicted resource had no new activity. Workload deltas remain exact,
but comparison and clustering reject the incomplete signature.

Rolling observations without a checkpoint track the most recent usage frame of
evicted resources and the frames of evicted submissions/copies. Their completeness
recovers once the requested window no longer overlaps pruned activity. An
all-history observation remains incomplete after pruning. Historical graph
queries such as `alive_at` return retained records only in live mode. IDs remain
session-local and must never be reused, as required by `ids.hpp`.

These limits bound history, not the application's live object population or
outstanding controlled GPU work. Trace schema 5 appends explicit command-list
and queue destruction events. Hosts notify before final COM release; a reused
pointer receives a fresh ID. Command-list destruction removes recording state
but preserves submitted residency batches. Queue retirement refuses unresolved
controlled work, polls its completion fence, and removes graph/runtime/native
registries plus unshared fence bindings only when safe. Live graph retirement
also removes per-resource queue evidence; an overlapping scene checkpoint is
invalidated conservatively instead of silently presenting complete evidence.

The pinned Wicked device destructor emits retirements after WaitForGPU, then
destroys its ARC bridge; later resource-release callbacks see no bridge. Hosts
with shorter object lifetimes must call the public retirement methods at those
lifetimes. Missing lifetime notifications cannot be safely guessed by ARC.
Tests cover 10,000 logical lifetimes, 1,000 COM pointer re-registrations, retained
submission history, shared native fence cleanup, and in-flight retirement
rejection. The new binary trace schema is deliberately rejected by older tools;
this reader expects schema 5 and does not silently reinterpret schema 4 files.

Regression coverage compares pruned checkpoint counters against full history,
checks destroyed-resource windows and expired-window rejection, runs 10,000
resource lifetimes with descriptor reuse, and verifies rolling-window recovery.
The churn test checks retained populations, not process RSS or allocator peaks.

The live residency event bridge also filters pending fence bookkeeping to
controlled resources. Repeated read-only submissions without fence signals do
not accumulate empty pending batches or external-resource use history. A
100,000-submission regression covers this case. A host granting residency
control after earlier read-only use must first synchronize that prior work;
control is an explicit safety opt-in, not retroactive tracking.
