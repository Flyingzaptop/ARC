# DX12 connection handoff — 2026-09-20

This increment fixes the launch/connection path, not automatic FPS gains.
Bodycam was not launched or controlled during this work. The user tests games.

## Findings

The old launcher held the first process ID even when that process exited. An
OpenProcess failure did not clear the PID, and old metrics could overwrite a
helper error. Receiving Presents after late attach did not establish shader
metadata coverage: the observed user session had no registered compute pipelines
and repeated `root_unknown` dispatch declines.

## Implemented

- The existing suspended startup path installs ARC before the application's
  entry thread resumes. A new startup helper mode also supports suspended children.
- When a launched application uses CreateProcessW for an executable within its
  configured installation directory, ARC prepares that child before its entry
  thread runs. The caller's CREATE_SUSPENDED flag and last error are preserved.
  Helpers outside that directory are not injected. Debug-created children are
  excluded. Handoff depth is bounded to four; initialization failures resume the
  caller's normally running child rather than preventing the game from launching.
- Each descendant receives a separate config, diagnostics and handoff record.
  The UI chooses a live registered descendant that actually produces Presents
  over the bootstrap PID, and clears dead PIDs. It does not guess a process from
  an unrelated executable name.
- Missing compute metadata now produces `render_metadata_missing`, with advice
  to restart through ARC. Unrelated automatic trials are skipped in this state.
- Root signature and compute-pipeline counts are exposed in diagnostics/UI.
  They are coverage indicators, not proof that an optimization is admissible.
- Stopping the optimizer also disables further child handoffs in that process.

## Verification

`tests/connection_launch_tests.py` runs a hidden 2x2 native DX12 fixture, not a
performance benchmark. Each renderer presents 100 frames over approximately
2.5 seconds. Results from `connection-handoff-02`:

| Case | ARC before main | Root/compute PSO counts | Unknown-root dispatches | Result |
|---|---:|---:|---:|---|
| Late attachment after resource creation | No | 0 / 0 | 100 | Correct explicit missing-metadata state; no trials |
| Direct suspended launch | Yes | 2 / 2 | 0 | Pass |
| Bootstrap exits after creating renderer | Yes, in child | 2 / 2 | 0 | Pass |
| Caller requests suspended child | Yes, in child | 2 / 2 | 0 | Pass; original suspend count preserved |

All cases have healthy devices and zero Present/hook failures. Release targets
built successfully and all 53 non-GPU CTest tests passed. The packaged binaries
are checked with the same connection fixture before delivery.

## Remaining limits

This does not reconstruct arbitrary pre-existing root signatures or original
shader bytecode. No positive automatic FPS gain is claimed. The complete GPU
cost/provenance gate and CPU overhead work remain unfinished.

A relaunch delegated to an already-running external launcher is not a descendant
created by the instrumented process and is not intercepted by this change.
Steam explicitly documents such a relaunch in
[SteamAPI_RestartAppIfNecessary](https://partner.steamgames.com/doc/sdk/api#SteamAPI_RestartAppIfNecessary).
CreateProcessA and other uninstrumented process-creation paths are also outside
this tested handoff path. Actual Bodycam process topology remains to be verified
from the user's next run. This is a development build, not full product acceptance.

The commit also preserves the preceding uncommitted development launcher/package
and raster-hook demand changes required by this working tree; those do not close
the remaining automatic optimization acceptance criteria.
