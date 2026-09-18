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

These limits bound historical populations, not the application's live object
population or every graph allocation. Command lists can be resubmitted after
closing; they cannot safely be discarded merely because they were submitted.
The event protocol currently has no command-list or queue destruction events,
so those registries can grow with unique object lifetimes. Completing that
lifecycle protocol is required before claiming a globally bounded runtime.

Regression coverage compares pruned checkpoint counters against full history,
checks destroyed-resource windows and expired-window rejection, runs 10,000
resource lifetimes with descriptor reuse, and verifies rolling-window recovery.
The churn test checks retained populations, not process RSS or allocator peaks.
