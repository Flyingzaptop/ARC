# Large packet / resident object data experiment

This is an explicit Wicked source adapter, **not automatic ARC offload**. Native CPU sorting is unchanged. `contract.json` specifies the measured scope.

`pack.hlsl` preserves the original 16-byte record order and emits the original 4-byte instance pointers. It computes group metadata on GPU with block scans; only 8,208 bytes return to CPU. Unsupported cases retain the original CPU loop. No production full-output readback or `WaitForGPU` is used.

Modes: `ARC_PACKET_MODE=0` original CPU loop; `1` full-output diagnostic oracle plus original loop; `2` real GPU replacement; `3` sidecar writes only. Modes 0/2/3 use the same executable and telemetry. New pass timings are published on the main thread in the **next frame**, not synchronously from render workers. The first live iteration exposed the old CSV writer's lack of thread safety; those raw timings are excluded.

Build `arc-packet-gpu` with CMake for the isolated replay. Run:

```powershell
python experiments/record-pack-gpu/test_replay.py build/packet-tests
python experiments/record-pack-gpu/instrument.py
# Build the owned Wicked Tests project with the repo's build-wicked-generic MT libraries.
python experiments/record-pack-gpu/run_live.py <new-absolute-output-directory>
python experiments/record-pack-gpu/analyze.py <output-directory> <analysis.json>
```

The instrumenter is deliberately checkout-specific and saves byte-exact backups in `build/packet-live-backup`. It refuses to overwrite changed source. The run script always restores the canonical executable; after collecting final patches, restore each of the five instrumented Wicked source files from that backup. Do not reset the external checkout: it contains earlier intentional changes.

The steady window is 6–19 seconds, reported separately from the entire active window after 4 seconds (which includes first-use compilation/allocation). Runs are capped at 60 seconds. Cadence is the interval between CPU Present invocations, **not display FPS**. GPU packet timestamps exclude CPU recording/waits; they must not be added to fence waits. The whole-frame comparison is the economic decision, not standalone shader time.

Existing engine GPU-frame queries do not include the newly early submitted work. They are not a complete GPU-frame measure for this experiment. NVIDIA power telemetry records observed values; the fixture does not force a power limit.
