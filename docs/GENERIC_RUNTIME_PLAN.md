# Generic runtime: closing the application adapter gap

The user requests automatic optimization in third-party DX12 applications. Game
validation remains user-owned; development validation uses ordinary native DX12
applications without cooperative ARC resource/action callbacks. No subagents.

The present probe observes counts only. This is not the requested optimizer.
Required execution chain:

1. Bounded object identities and destruction notifications, descriptor versions,
   command recordings and actual submitted work, with explicit coverage gaps.
2. Generic capability discovery from those observations. Bindless membership is
   only a possible access; unknown state cannot authorize mutation.
3. Present/queue association, bounded asynchronous GPU readback/timestamps and
   trustworthy comparable references. Never block every Present to run F trials.
4. Independently verified reversible actions through DX12 mechanisms, preserving
   synchronization and descriptor volatility contracts. Reference/identity loss
   must revoke capabilities and restore accepted actions.
5. Unmodified-application launch/capture/optimization validation and user-facing
   packaging. A successfully injected DLL is not optimization acceptance.

First implementation slice connects observed object lifetimes, descriptor contents,
copy/render operations and submission order into the existing attribution graph.
Captures are bounded and explicit, not always-on per-draw graph reconstruction.
Private COM lifetime tokens avoid retaining resources or counting reused native
pointers as the same object. All native calls preserve original arguments/results.
Invalid or missing state is recorded as incomplete. Unsupported APIs remain visible
as coverage limits. Original counter-only mode remains available.

Completion must be reported against the entire chain above, not just this slice.
No automatic game mutation is claimed until the actual generic capability/reference
path reaches the existing independent critic and rollback controller.
