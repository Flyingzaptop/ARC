# Automatically found CPU producer

This extends the native-event data correspondence workflow inside the **same live process**. No source oracle, engine field/function name, prepared RVA or fixed object address is an input to the investigator.

The write watcher finds a machine store using a hardware data breakpoint. The planner decodes the captured runtime function (bounded static bytes), identifies the store by effective address, and proposes a short predecessor region. The slice observer records only that region, on previously observed writer threads and matching validated array elements. It does not trace the parent function or whole process. The independent replay checks output bytes on new inputs.

Build the `arc-cpu-producer-watch`, `arc-cpu-producer-slice` and `arc-dx12-probe-launch` targets. Build the owned host after applying `experiments/gpu-candidate-observer/instrument.py`; the observer now supports bounded follow-up witnesses and QPC timestamps. Save that host outside its canonical EXE location, then:

```powershell
python experiments/cpu-producer/run.py <new-output> <saved-observer-host.exe> --watch-events 6 --watch-ms 3000 --slice-calls 3 --slice-ms 5000 --event-buffer-budget-kib 1024
```

The memory setting is specifically the native event-buffer reservation, not total game/ARC memory. The fixed x64 reservations total 770,048 bytes; a lower requested budget is rejected. Static-code and per-memory-read limits are in `contract.json`. Injection output paths must not exist beforehand.

Offline reproduction needs Python and Capstone 5.0.6, but no GPU or Windows execution:

```powershell
python scripts/analyze_cpu_producer.py <extracted-raw-root> <result.json>
python scripts/cpu_producer_replay.py <extracted-raw-root>/slice <replay.json>
python tests/cpu_producer_tests.py
```

The observer's follow-up file contains only the discovered resource/generation and a capture limit, not semantic fields. Original discovery stays limited to its first three observations; later snapshots are witnesses, not training data for refitting.

Keep `replacement_allowed=false`. Exact output replay does not establish memory ownership, all flags/vector-register effects, first consumers, iteration independence or profitable offload. The retained handler/DLL is suitable for this bounded training process, not a production hot-unload mechanism.

The runner restores the canonical EXE. After saving the host patch, restore external `wiGraphicsDevice_DX12.cpp` and `wiRenderer.cpp` from `build/gpu-observer-backup`; do not reset the external checkout wholesale.
