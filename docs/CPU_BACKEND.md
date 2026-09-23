# CPU backend: M2–M4 vertical slice

This backend is independent of the game engine. Its initial supported domain is
small; it is not a universal function optimizer or a LOD controller.

## Execution boundary

The CPU session launches the target through pinned Windows x64 DynamoRIO. It is
separate from the DX12 DLL session. No engine adapters, function names, fixture
addresses or manually supplied dependency graphs select candidates. The decoder
selects supported straight-line basic-block prefixes in the main executable.
Symbols can identify test oracles after discovery, never select the region.

The native decoder initially admits 64-bit register MOV and LEA. These operations
do not dereference memory, modify flags or SIMD state, or call external code.
Stack-pointer writes and unsupported address forms decline. All written registers
are conservatively live outputs. Unknown operations remain original instructions.
The IR can model additional integer arithmetic with an explicit flags-dead proof,
but that does not expand the current native decoder's supported set.

The executor observes current register state before effects and skips a prefix
only when its contract and guards pass. Original execution remains available at
the same boundary. A changed generation or user stop prevents new admissions. Protection changes are range-scoped; restored code receives a fresh publication. A 64-slot recency/activity pool admits late hot code and permits evicted code to return.
This first implementation uses a clean-call execution boundary; DBI and transition
costs can exceed the removed arithmetic. Execution counts are not speedup evidence.

## Three distinct actions

- Specialization derives constant nodes from previously observed stable register
  inputs, then checks the input guard before reuse. Observation alone is not proof.
- Exact memoization compares all extracted live-in register values, not hashes or
  pointer identity. Four cache entries per retained region bound storage.
- Incremental evaluation extracts an acyclic dependency slice from decoded
  instructions. It retains intermediate values and recomputes affected nodes when
  a live-in changes. The fixture does not supply that graph.

Memory-backed reuse, arbitrary loops, calls, syscalls, atomics, hidden TLS state,
floating-point transformations and arbitrary function replacement are unsupported.
A pointed-to input cannot cause a false cache hit because a dereferencing region
does not enter this supported class. This is a decline, not implemented memory
tracking. Module/code generations and native backend checks remain necessary even
for register-only regions.

## Session modes and evidence

`scripts/arc-cpu-session.ps1` provides `baseline`, `neutral`, `study` and `apply`.
The first launches natively; the others have separately identified DBI costs.
The package provides matching `.cmd` entry points with an EXE picker. Select the
actual target executable; child-process inheritance is disabled by default.

`apply/auto` now takes nine interleaved timing samples per action (original,
specialize, memo, incremental), including guard failures and redirects. Profit
uses the arithmetic mean of **all** calls, so expensive misses are not discarded.
Median/IQR are only noise diagnostics. Measured timer, tracking and amortized cold
admission costs are also charged. Probing is bounded and repeated after 512 calls. Missing
clock/completion evidence or no positive margin keeps original execution.
The scope is **within DBI with the same timing boundary**, not a no-ARC FPS claim.
Each reported policy is a last-thread snapshot, not process-wide consensus.
Cost histories have separate fixed storage for all 64 candidate slots per thread;
evicting one of the four data caches does not restart calibration. A new candidate
generation does invalidate its old cost evidence.
Explicit `-Actuator specialize`, `memo` or `incremental` remains diagnostic.

The output folder records executable/client/runner identities, mode and lifetime.
Its `stop.ps1` requests disabling CPU transformations without killing the game.
`STOP-ARC.cmd` in the same directory provides the same request by double-click.
Final runtime evidence is written when the application exits normally. A crash or
missing report is explicitly incomplete. Commercial-game checks belong to the user.

`scripts/capture-cpu-work.ps1` captures scheduling, sample, module and presentation
events in one ETL. Its analyzer ranks stable module-relative ranges as study
proposals. Sampling shares estimate cost; they do not prove semantics or identify
geometry. Missing clocks, process lifetime, events or GPU duration remain unknown.
ETW collection requires normal Windows elevation and is optional for CPU launch.

## Build and package

The DBI backend has a separate CMake build so the graphics DLL does not acquire a
DynamoRIO dependency. The pinned dependency and redistribution license are recorded
in `config/cpu-backend-dependency.json`; the offline package includes the license.
`scripts/package-cpu.py` packages only already built binaries and records file
hashes and source identity. It never silently installs dependencies into a game.

```powershell
cmake -S src/backends/cpu -B build/cpu_backend -G "Visual Studio 18 2026" -A x64 -DDynamoRIO_DIR="<DynamoRIO-Windows-11.3.0-1>/cmake"
cmake --build build/cpu_backend --config Release --target arc_cpu_client cpu_native_fixture
python scripts/package-cpu.py "<fresh package directory>" --dynamorio "<DynamoRIO-Windows-11.3.0-1>" --client build/cpu_backend/Release/arc_cpu_client.dll --fixture build/cpu_backend/Release/cpu_native_fixture.exe
```

See `ARC_IMPLEMENTATION_STATUS.md` and the M2–M4 evidence record for actual checks,
current limitations, measured costs and remaining game acceptance.
