# Native-event candidate discovery

The observer core takes native D3D12 events. It contains no scene, field, engine name, target address or exact target buffer size. Five seconds is a generic observation warmup; candidates are then ranked by repeated upload traffic and available read-intent/binding evidence. At most two buffers, three observations each, and 128 KiB per observed write are read back.

`observer.hpp` records the readback **immediately after the observed write** and restores COPY_DEST state. A fence is signalled after the corresponding native list submission. No `WaitForGPU` or CPU fence wait is used. CPU memory collection/file writing remains expensive training instrumentation.

`scripts/gpu_candidate_pipeline.py` validates provenance before calling the existing byte-correspondence detector on observations 0/1. Observation 2 is only a holdout. A failed holdout does not trigger retraining on it. Destruction/aliasing or changed generation/range cancels the old binding. Confirmation is historical evidence; a binding destroyed at process shutdown correctly reports `live_binding_valid=false`.

`instrument.py` inserts event delivery at native API boundaries in the owned fixture. Its separate oracle hook uses source names **only to check the answer afterwards**. Neither `run.py` nor the automatic pipeline reads `oracle.jsonl`; `verify.py` does that independently.

```powershell
python experiments/gpu-candidate-observer/instrument.py
# Build owned Wicked Tests against build-wicked-generic.
python experiments/gpu-candidate-observer/run.py <new-output-directory>
python experiments/gpu-candidate-observer/verify.py <output-directory> <oracle-result.json>
python tests/gpu_candidate_pipeline_tests.py
python tests/cpu_gpu_correspondence_tests.py
```

Restore the two external source files from `build/gpu-observer-backup` after saving patches. The runner restores the canonical EXE in `finally`. Observer bookkeeping/readbacks are deliberately process-lifetime allocations in this bounded training process so late resource-destruction notifications cannot access already-destroyed static state. They are not a production session manager.

The generic CPU sampler inventories private RW regions. No CPU allocation lifetime proof, automatic shader semantics, transfer code generation or game acceleration is claimed. See `contract.json` for exact budgets and coverage limitations.
