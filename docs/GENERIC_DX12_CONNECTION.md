# Generic DX12 connection — experimental read-only boundary

Build with `-DARC_GENERIC_DX12_PROBE=ON` on Windows x64. Targets:
`arc-dx12-probe.dll` and `arc-dx12-probe-launch.exe`.

The launcher can start an explicitly supplied executable or attach to an explicitly
supplied authorized PID (`--attach PID DLL OUTPUT`). It loads the observer into
that process using ordinary Windows APIs. It does not elevate, alter system
security, replace game files, or bypass protected processes. Permission/init
failures are errors, not a reason to disable protections. Use only permitted targets.

The observer finds DX12/DXGI implementations using its own hidden bootstrap
device/swapchain and installs process-local MinHook detours. All original arguments
and return values are preserved. Render callbacks only increment bounded atomic
counters; a worker writes periodic snapshots. Bootstrap calls are excluded.
Runtime modules and the observer remain loaded until process exit; live unloading
is not supported. A process accepts one probe instance. Exit the target to remove it.

Counters cover Present/Present1, queue submissions, direct draw/indexed draw,
dispatch and base CreateCommittedResource. Alternate implementations, newer
allocation APIs, indirect/mesh work and missed calls remain explicitly outside
coverage. These are **partial API counts**, not a reconstructed resource graph or
GPU timing. DLL initialization alone is not a game-compatibility PASS.

## Optimization boundary

The same portable admission policy is linked, but unknown games have no verified
reference-replay or safe resource-mutation capability. It therefore abstains and
reports `observe_only`, `safe_mutation_capability=false`, `quality_mutations=0`.
This module does not claim that F/G can already optimize arbitrary games.
The missing engineering is a sufficiently complete live resource/work bridge plus
safe generic actions and independently comparable reference captures.

## Local evidence

On the separate native DX12 smoke process, the probe observed tens of thousands of
Presents/submissions and hundreds of thousands of draw calls with no hook or
Present failures. Its first launcher attempt exposed transient Windows module-
snapshot `ERROR_BAD_LENGTH`; retry now handles that documented transient.

An initial FPV.SkyDive launch observed real DX12 calls but exited because Steam
initialization failed. After the user signed in manually, the game was launched
normally through Steam and the probe attached to the explicit game PID. Solo
freestyle on Abandoned Factory produced sustained game Present, submission and
indexed-draw observations without any engine-specific runtime code.

The first 238-second scene window observed one `0x887a0001` Present failure while
the game continued running; its strict zero-error acceptance is retained as FAIL.
An uninstrumented control and a later instrumented run did not reproduce it,
including windowed/fullscreen transitions. The original failure is not relabeled
PASS or attributed to the game without proof. Native HRESULT/flags/device-reason
diagnostics were added for a future recurrence. No login dialog is automated and
no game DRM or authentication code is modified.

The Unity launch argument selects the application's existing graphics backend;
there are no Unity APIs or engine names in the observer or core. Other DX12 targets
do not need that argument.

References: [MinHook](https://github.com/TsudaKageyu/minhook),
[Windows CreateRemoteThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createremotethread),
[Unity 2022.3 player arguments](https://docs.unity3d.com/2022.3/Documentation/Manual/PlayerCommandLineArguments.html).
