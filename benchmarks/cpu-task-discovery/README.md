# CPU automatic-discovery gate

This is a bounded observation/triage implementation, **not automatic GPU offload**.
No application name, symbol, known target address or engine source selects candidates.
The existing Wicked fixture is used only to choose a workload and measure frames.

Build `arc-cpu-task-probe` in the main Release CMake tree. Install the pinned Python
requirements in this directory. On the existing benchmark installation:

```powershell
C:/Python314/python.exe scripts/run-cpu-task-discovery.py C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-discovery-repeat
```

The runner temporarily uses the measured host saved by `1c8a51e`, runs original
execution and bounded discovery, and restores the prior executable. It never launches
commercial games. Each host run is 20 seconds, with an 8-second warmup. The 10-second
probe starts 8 seconds after process creation. Startup clocks differ slightly: raw
manifests preserve the launch offset; do not equate these timestamps precisely.

For an already authorized process, the native tool accepts PID, duration 1–12 seconds,
and a new output directory. It enumerates thread cycle/CPU counters, prioritizes active
threads and includes the visible-window owner as a **hint**, not a proven frame thread.
Every 100 ms it may sample up to four contexts. Each suspension is immediately undone
by RAII. A suspension above 5 ms or aggregate suspension time above 50 ms disables
further contexts; lightweight counters continue until the bounded end. This is not
zero-overhead profiling and it is not ETW Running/Ready/Waiting attribution.

The probe preserves the exact on-disk PE alongside live executable-section snapshots.
Analysis checks function bytes against that generation before decoding. Exception-directory
function boundaries do not require PDBs. Leaf code without those boundaries remains
unresolved. Top 32 sampled functions, 64 KiB/function, 4 KiB/backedge span, 64 candidate
spans maximum. Overlapping backward-branch spans are not independent tasks or proven
natural loops. Prospective effective addresses from sampled register contexts are not
complete observed memory footprints.

```powershell
C:/Python314/python.exe scripts/discover-cpu-tasks.py <probe-directory>
C:/Python314/python.exe tests/cpu_task_discovery_tests.py
```

An independent array-map shape screen checks memory operations, candidate induction
steps, branches, calls, atomics and register recurrences. The safety gate deliberately
requires facts this sampler cannot establish: invocation entry/exit bounds, ownership,
complete alias/dependence closure, external effects and first consumers/deadlines.
It cannot authorize replacement. Runtime cost, invocation frequency and profitable
batch size remain null rather than being guessed from sample counts.

The current run found candidate spans but no admitted task. Therefore no GPU primitive,
replacement, adaptive scheduler or pretend offload performance arm was built. The next
missing channel is selected-region entry/exit and memory/dataflow instrumentation,
including callees and thread/job ownership, with measured overhead. Whole-program DBI
is not enabled as a substitute. Historical M0–M4 and the source-assisted adapter remain
unchanged.
