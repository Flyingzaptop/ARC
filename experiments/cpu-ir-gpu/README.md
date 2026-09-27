# Captured scalar IR to GPU

`scripts/cpu_ir_gpu.py` lowers the existing `cpu_producer_replay.py` output provenance graph. It accepts only complete scalar float32 leaves and `vaddss`/`vmulss` nodes. It assigns input slots by first graph use within each captured call, checks that all calls have the same topology, evaluates each one with float32 rounding after every operation, and requires byte equality with captured outputs. Unsupported leaves, mixed byte provenance, incomplete output maps, non-normal values, and divergent graphs fail closed. Captured memory values, including values that happened to repeat, remain explicit inputs; immutability has not been established.

Generate the saved 768-element three-group fixture and the final 256-element held-out group:

```powershell
python scripts/cpu_ir_gpu.py docs/evidence/cpu-producer-20260927/result.json build/cpu-ir-gpu/fixture --packet-root C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-batch-final-20260927
python scripts/cpu_ir_gpu.py docs/evidence/cpu-producer-20260927/result.json build/cpu-ir-gpu/heldout-256 --packet-root C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-batch-final-20260927 --packet-group 2
```

The first command's pooled 768 elements span separate observed packets. It is useful for isolated throughput, not evidence that those packets can be merged during execution. The held-out 256-element group reflects one observed local packet. The generator uses the packet analysis's affine relocation check and verifies every expected output.

From a Visual Studio x64 developer shell, build the generic isolated harness without modifying ARC's CMake configuration:

```bat
cl /nologo /std:c++20 /EHsc /O2 /Fe:build\cpu-ir-gpu\bench.exe experiments\cpu-ir-gpu\bench.cpp d3d12.lib dxgi.lib d3dcompiler.lib
build\cpu-ir-gpu\bench.exe build\cpu-ir-gpu\heldout-256\generated.hlsl build\cpu-ir-gpu\heldout-256\input.bin build\cpu-ir-gpu\heldout-256\expected.bin 256 7 3 build\cpu-ir-gpu\heldout-256.csv
build\cpu-ir-gpu\bench.exe build\cpu-ir-gpu\heldout-256\generated.hlsl build\cpu-ir-gpu\heldout-256\input.bin build\cpu-ir-gpu\heldout-256\expected.bin 256 7 3 build\cpu-ir-gpu\heldout-256-gather.csv build\cpu-ir-gpu\heldout-256\gather.bin
```

The first run uses offline packed inputs. The second uses `gather.bin`: original-stride `input_ready` snapshots, graph-derived offsets, and separately captured shared words. It packs inputs into the upload buffer on every iteration, charging that work to `prep_ms`. These are snapshots; the file says nothing about live buffer lifetime or ownership. The CSV separates CPU preparation, command recording, submission, fence wait, and CPU result consumption from GPU upload, dispatch, and return timestamps. `full_ms` already includes the wait; GPU times must not be added to it. The offline Python evaluator is the reference for the generated expression only. Neither it nor this harness times removable original game work, renderer contention, or frame impact. Exact output equality on the tested GPU is required before accepting the generated precision class. The captured producer inputs change inside the callback and a CPU consumer reads the output there, so this fragment is not eligible for standalone live replacement.

For a narrower CPU reference, `original_slice.py` validates the captured straight-line x64 arithmetic span mechanically: one RIP literal, six bounded source reads, 12 bytes of scratch writes and reads, and two bounded output writes. It rejects branches, calls, different register effects, or other memory accesses. The thunk saves nonvolatile registers and runs against owned snapshot buffers. Generate and build it separately:

```bat
python experiments\cpu-ir-gpu\original_slice.py C:\Users\r3d_flzp\ARC-Hardening-GPU\universal-optimizer\cpu-batch-final-20260927 build\cpu-ir-gpu\original-slice
cl /nologo /std:c++20 /EHsc /O2 /Fe:build\cpu-ir-gpu\original-bench.exe experiments\cpu-ir-gpu\original_bench.cpp bcrypt.lib
build\cpu-ir-gpu\original-bench.exe build\cpu-ir-gpu\original-slice\original-slice.bin build\cpu-ir-gpu\heldout-256\gather.bin build\cpu-ir-gpu\heldout-256\expected.bin 256 bfd5e51142f1a6c4f9b4b948253268bb82d71d1c74650e20d1b3acfe4fb0c8d7 build\cpu-ir-gpu\original-slice-256.csv
```

The CPU timer measures calls to the original captured arithmetic instructions for one observed 256-element packet. It excludes upstream input production, earlier branching, the immediate CPU consumer, and scheduling effects. The GPU timer's full route includes transfer and wait. These figures can bound the economics of the isolated fragment but cannot be converted directly into saved frame time.
