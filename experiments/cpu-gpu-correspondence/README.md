# One automatic correspondence in training mode

`scripts/cpu_gpu_correspondence.py` learns an affine 12-byte float-vector correspondence from two raw CPU/GPU observations. No engine names, field names, supplied strides, known CPU object addresses, or oracle metadata enter its input. Region identifiers are opaque. It rejects unchanged constants and validates an unused range of record indices across both frames. Harmonic/subsample aliases are marked, not counted as separate discovered tasks.

The result is an **observed exact correspondence**, not a semantic/dataflow proof, lifetime proof or permission to replace a CPU task. Bounds: 64 CPU windows of 256 KiB, GPU snapshots <=32 MiB, seed search in first 4 KiB, inferred GPU stride <=1024 and CPU stride <=4096, time budget 60 seconds. Other representations/conversions/layouts are unsupported.

Capture is intentionally not claimed fully automatic. `memory_windows.hpp` enumerates committed private RW memory windows generically. The fixture still selects `instanceBuffer` for completed GPU readback; its existing mapped upload and the manually assembled audit/reference arrays are excluded from CPU candidates. This manual GPU-candidate selection is a remaining link. CPU object field/layout is not provided to the learner.

`gpu_audit.hpp` writes a separate source-assisted oracle. `make_input.py` strips all its semantic metadata; the learner opens only files listed in `input.json`. `verify.py` is run **after** discovery to compare the answer. `blind_replay.py` renames real input files and replaces addresses used as identifiers with UUIDs, checking identical inferred relations.

Native setup: apply `record-pack-indirect/instrument.py`, then `install_capture.py`, build owned Wicked Tests. `run_training.py <new-output>` runs one busy-slot diagnostic and one bounded capture, then automatically builds the anonymous input and discovers correspondences. No commercial game is launched. Canonical EXE is restored in `finally`; restore five external source files from `build/indirect-live-backup`, and the bridge from `build/correspondence-backup` after saving patches.

The GPU capture is a slow diagnostic: it uses completed readback and `WaitForGPU`. It is separate from the busy-slot test and is not installed in the default performance path. The latter uses a real delayed GPU consumer reading all three versions' meaningful uploads, packed output and draw arguments, checking byte preservation before slots can resume.

Reproduce from evidence:

```powershell
python scripts/cpu_gpu_correspondence.py <capture/input.json> <result.json>
python experiments/cpu-gpu-correspondence/verify.py <result.json> <capture> <oracle-result.json>
python experiments/cpu-gpu-correspondence/check_busy.py <busy/frames.csv> <busy-result.json>
python tests/cpu_gpu_correspondence_tests.py
```
