# Composite CPU path scratch GPU trial

`scripts/cpu_composite_gpu.py` lowers a closed observed typed IR path and a
captured append contract to a generated compute shader. It packs captured
memory and register leaves into ordinal input slots. The output for each
element is one validity word, a 16-byte scratch record, and a 64-bit cursor
advance. Captured process addresses never enter the shader.

The generated `input.bin`, `expected.bin`, `generated.hlsl`, and `contract.json`
can be passed to the copied `bench.cpp` harness with its usual arguments:

```
python scripts/cpu_composite_gpu.py --capture CAPTURED_CANDIDATE_DIRECTORY \
  --append-capture DIRECT_CALLEE_DIRECTORY --out OUTPUT_DIRECTORY
```

For the bound bulk fixture, add `--bulk-proof FINAL_PROOF.json`,
`--bulk-input PACKED_BEFORE_VIEW.bin`, and
`--bulk-expected-records INDEPENDENT_AFTER_VIEW.bin` to the command.

```
bench.exe generated.hlsl input.bin expected.bin COUNT INPUT_WORDS OUTPUT_WORDS timings.csv
```

The benchmark compares every output word, including validity and cursor
advance. The early 32-row fixtures bind captured live-ins individually.
`covered-v2-bulk/` is the later 65,344-row positive-packet fixture: a bounded
native gather packs 31 ordinal input words per row from captured before views;
the generated shader omits per-row carried cursor and capacity inputs after a
maximum-count no-growth preflight and a checked terminal-loop proof. Its
expected records come independently from the after view. The full-packet GPU
evaluation and metadata passed exact isolated byte comparison after explicit
round-to-nearest-even FP16 lowering.

The generator requires the full ordered output footprint to agree with the
append contract. It rejects unmatched writes, differing packet topologies,
unsupported operations, and unlowerable branch guards. Pointer-only cursor
values are represented as a checked relative advance, while record data and
metadata remain explicit in the scratch output.

Runtime sampling and machine-code analysis identified and ranked candidate
regions without engine names or supplied addresses. This particular region
was then selected for bounded study; capture setup, independent CPU replay,
and GPU trial execution were explicitly orchestrated on the local stand. The
typed IR and HLSL for the supported path were generated from captured
instructions and provenance, not handwritten as an engine adapter. The
snapshot binding and guards apply to this observed positive path; alternate
paths, live ownership, and consumer publication are not established.

`compaction/` contains a separate three-dispatch GPU prefix/scatter primitive
and isolated holdout and mixed fixtures. It reads scratch validity and cursor
delta for each row. Validity 1 with delta equal to record stride emits;
validity 1 with delta 0 is a proven rejection; a failed validity guard or an
unexpected delta increments `invalid_count`. The primitive assigns output
slots from a GPU exclusive prefix and computes the final cursor from one
initial cursor. Its four metadata words are emitted count, final cursor low,
final cursor high, and invalid count. Results must not be published when
`invalid_count` is nonzero.

The isolated `compaction_bench.cpp` harness accepts the three shader files,
scratch bytes, packed expected bytes, metadata expected bytes, row count,
scratch words, record words, initial cursor, and CSV path. It includes upload,
three dispatches, wait, readback, and validation. Runtime enforcement of the
demonstrated no-growth preflight, live input binding, and mixed filter path
reconstruction are still required before connecting this primitive to ARC.

`fused_bench.cpp` measures the complete isolated GPU tail in one command list:
packed input upload, generated expression evaluation into GPU scratch, local
prefix, group prefix, scatter, and final packed-record and metadata readback.
It does not read scratch back between stages. It takes the generated shader,
compaction shader directory, packed input, independent packed expected records,
16-byte metadata expected, count, input words, scratch words, record words,
initial cursor, and CSV path. Compiler and device setup occur before timing;
byte comparison occurs after the measured CPU consume copy. The CPU snapshot
gather is measured separately and must be added to the fused tail as a
nonoverlapped composed cost. GPU timestamps are already inside the measured
wait and must not be added a second time.

The 65,344-row fused tail passed exact isolated validation. Its 20-run median
was 1.46115 ms; three saved bounded-gather reruns were 3.2715, 3.2113, and
3.4299 ms (median 3.2715 ms). The composed nonoverlapped estimate is 4.73265
ms by medians and 4.80409 ms by means. The earlier single gather was 3.1998
ms and is retained separately. The recorded warm original CPU samples have
mean 3.34356 ms (including an 8.4059 ms outlier) and median 2.0819 ms.
Even against that CPU mean, the isolated GPU route is slower. The output
region's captured protection is 0x4 (ordinary write-back), so write-combine
preservation is not applicable here. These are loop-route timings, not a
measured change in complete frame duration; live replacement remains disabled.
Recompute the comparison from saved raw CPU, gather, and GPU files with
`python scripts/analyze_cpu_composite_cost.py append experiments/cpu-composite-gpu/results --check`.
