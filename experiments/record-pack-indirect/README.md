# Versioned, asynchronous indirect packet experiment

Wicked source-assisted adapter; not automatic ARC discovery or universal admission.

Modes: `ARC_INDIRECT_MODE=0` original CPU packing; `1` real asynchronous indirect replacement; `2` the same indirect rendering plus deferred exact diagnostics. Unsupported calls retain CPU packing. CPU sorting and its equal-key order are unchanged.

`contract.json` maps every metadata field to its consumer. This stage supports **one group**: CPU obtains mesh/LOD/stencil from sorted records and current compact producer data, proves force-alpha false, and retains original material/PSO selection. GPU creates `DrawIndexedInstancedIndirect` arguments. The group's offset is zero; this is not support for arbitrary multi-group offsets.

`admission.hpp` is shared by runtime and the native admission/retirement tests. Run `build/Release/arc-indirect-admission-tests.exe` after building that CMake target.

The fixture has three versions of frame uploads and two pass slots per version. A dedicated fence retires **all frame consumers**, after native submissions and cross-queue waits. Compute completion alone never authorizes reuse. Busy slots use the CPU path. There is no metadata fence wait in the render path. The inherited `Worker::wait` helper is unused by this adapter.

The full queue-envelope query starts before the early instance upload and ends after frame queues complete. It includes GPU idle time/CPU starvation; do not present it as active GPU execution time. Separate timestamps measure packet compute and input copy. Performance mode returns timestamps only. Output, metadata and indirect arguments are read back only in diagnostic mode, after the consumer fence. Diagnostic CPU reference generation is absent in performance mode.

```powershell
python experiments/record-pack-indirect/instrument.py
# Build the owned Wicked Tests project against build-wicked-generic.
python experiments/record-pack-indirect/run_live.py <new-absolute-output-directory>
python experiments/record-pack-indirect/analyze.py <output-directory> <analysis.json>
```

`instrument.py` saves byte-exact originals under `build/indirect-live-backup` and refuses to overwrite modified files. `run_live.py` caps processes at 60 seconds and restores the canonical executable in `finally`. Restore the five external source files from the backup after saving their diffs; never reset that checkout wholesale.

The runner uses one binary for CPU/indirect comparisons and records GPU clocks, temperature and power. It does not force a power limit. Steady samples belong to frames completing between 6 and 19 seconds; deferred events keep their original frame IDs. Raw startup/slow frames remain in the archive. Present-call cadence is not display FPS.
