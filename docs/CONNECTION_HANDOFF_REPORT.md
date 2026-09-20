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

## Steam handoff correction (development package 03)

The user's next run established the actual topology: the ARC-launched PID 29656
exited; the already-running Steam process started Bodycam.exe (PID 2748), which
started the renderer (PID 24612). No child handoff record was created in the ARC
session. This confirms the external-launcher case above, not a failure of the
tested in-process CreateProcessW handoff.

The launcher now accepts `--steam-launch <FPS> <original EXE> [original arguments]`.
The new **Строка для Steam** button copies a launch-option wrapper ending in
`%command%`. Steam supplies its original executable and arguments; ARC preserves
them and the inherited Steam environment while applying suspended initialization.
The original executable is not replaced with a guessed Shipping binary.
The user pastes the line in the game's Steam launch options and starts it in Steam.
Removing that wrapper restores the original launch route.

`tests/steam_wrapper_launch_tests.py` verified the packaged GUI entry point using
an owned hidden DX12 fixture, both directly and via a bootstrap. Both runs observed
100 Presents, registered compute metadata, and no unknown-root dispatches. The
test also verified an inherited SteamAppId marker, quoted/spaced/trailing-backslash
arguments, and the requested target FPS. This is a synthetic Steam context test,
not a live Steam or Bodycam success claim. The user performs that final check.

The dead-process UI now clears its stale PID/details and directs Steam users to
the wrapper route rather than indefinitely suggesting another direct launch.

## Maximum-FPS mode and descriptor capacity (development package 04)

The user requested maximizing FPS rather than stopping at a numerical target.
Launcher sessions now explicitly enable `maximize_fps`; the numerical target UI
is replaced with the maximum-FPS mode label. Old Steam command lines still parse
their numeric argument, but it does not stop maximum-mode search. The legacy
target policy remains available to explicit diagnostic configurations.

Maximum mode continues discovery and accepts measured quality-qualified gains
even above the old target. It does not restore an accepted setting merely because
FPS is high. Quality thresholds, evidence expiry and overhead budgets are unchanged.
After all trials are exhausted with no active setting, discovery repeats no more
often than every 15 seconds. A sustained large frame-period change resets the
scene's decision history, so loading-screen decisions do not certify gameplay.

The Bodycam session had 4412 successful intercepted Presents but its automatic
session faulted at 929 samples: `descriptor ranges unavailable`, then `Optimizer
bundle refused`. Descriptor-copy capacity limits now invalidate cached contents
(including the exact view cache) and record a coverage limitation rather than
throwing a fatal optimizer exception. Heap identities survive invalidation.
The tracked range count increased from 1024 to 65536; per-call descriptor-count
bounds remain. This is not a claim of unlimited metadata capacity.

The live compute PSO count ceiling increased from 512 to 16384; the 64-MiB pending
shader-bytecode budget remains. This removes the observed 512-entry barrier but
does not implement unbounded or disk-backed shader capture.

The native connection fixture now retains 600 distinct additional PSOs and performs
a real CopyDescriptors call with 1025 source ranges. Early direct/child/suspended
child runs register 602 compute PSOs with zero optimizer faults or unknown roots.
Unit tests prove gain acceptance above target, preserved quality rejection, and
safe descriptor invalidation/recovery. These are correctness tests, not FPS-gain
measurements. Swapchain recreation and full GPU cost/provenance acceptance remain
separate work; no Bodycam fullscreen-transition fix or automatic gain is claimed.
