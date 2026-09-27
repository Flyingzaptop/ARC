# CPU composite evidence (2026-09-27)

`raw.zip` contains bounded traces, isolated inputs/results, and **only exact-range-redacted VM snapshots** for the two bulk candidates. The original runtime heap images are not in this archive. Redacted region files retain their original sizes and address offsets; unneeded bytes are zero. Both redacted copies passed the owned native full-loop replay before packaging. `manifest.json` lists every archived member's size and SHA256; each capture's `redaction.json` binds the copied regions to original and redacted SHA256 hashes and retained-byte counts. `stand-restoration.json` records the owned stand's restoration check.

Extract `raw.zip` to a fresh directory. Set these variables to paths within that extraction:

```powershell
$env:ARC_COMPOSITE_EVIDENCE_ROOT = 'C:\path\to\extract\captures'
$env:ARC_COMPOSITE_ARTIFACT_ROOT = 'C:\path\to\extract\artifacts\cpu-ir-gpu'
```

From the repository root, run the portable suites:

```powershell
$tests = @('cpu_capture_memory_tests.py','cpu_path_filter_tests.py','cpu_ir_gpu_tests.py','cpu_append_contract_tests.py','cpu_composite_ir_tests.py','cpu_composite_gather_tests.py','cpu_composite_gpu_tests.py','cpu_snapshot_gather_tests.py','cpu_word_gpu_tests.py','cpu_word_snapshot_tests.py','cpu_snapshot_redact_tests.py','cpu_producer_tests.py')
foreach ($test in $tests) { python (Join-Path 'tests' $test); if ($LASTEXITCODE) { throw "$test failed" } }
```

The bulk captures are at `captures/cpu-composite-bulk-covered-v2-20260927/candidate-01` and `captures/cpu-composite-pack-bulk-20260927/candidate-00`. Their `views.json` and packaged native manifests use relative region filenames. The code-only `module-image.bin` is a bounded native replay dependency; it is separate from the redacted VM views. Numeric addresses are evidence from the captured run, not reusable live addresses.

To regenerate the second candidate's 32-path model and counted bulk proof from extracted captures:

```powershell
$train = Join-Path $env:ARC_COMPOSITE_EVIDENCE_ROOT 'cpu-composite-pack-train-20260927\candidate-00'
$bulk = Join-Path $env:ARC_COMPOSITE_EVIDENCE_ROOT 'cpu-composite-pack-bulk-20260927\candidate-00'
python scripts/cpu_composite_ir.py $train model32.json
python -c 'import os,pathlib; from sys import path; path.insert(0,"scripts"); from cpu_word_snapshot import packet_proofs,gather_manifest; c=pathlib.Path(os.environ["ARC_COMPOSITE_EVIDENCE_ROOT"])/"cpu-composite-pack-bulk-20260927"/"candidate-00"; o=pathlib.Path("word-proof-new"); p=packet_proofs(c,o); gather_manifest(c,p,o/"gather-manifest.json")'
```

On Windows with Visual Studio x64 build tools, the bounded native gather source can be regenerated from that new proof and the checked static shader slot contract:

```powershell
python scripts/cpu_snapshot_gather.py $bulk experiments/cpu-word-gpu/static-plan/contract.json word-proof-new/gather-manifest.json word-gather-new --emit-native --optimized --separate-diagnostics --packet-kind word_scatter --word-proof word-proof-new/proof.json
```

Compile `word-gather-new/gather.cpp` from an x64 developer shell, then run its executable with `word-gather-new/input.bin` and `word-gather-new/source-ranges.csv` as its two arguments. It checks every load against an owned snapshot view, rejects output aliasing, and compares the first two packed rows with the traced leaves. The saved archive already includes the 65,343-row input, independent after-view words, source ranges, proofs, native measurement JSON, and isolated GPU result CSVs. There is no need to rerun a benchmark to inspect the result.

The hashes in `manifest.json` apply to the **exact archived bytes**. Regenerated proof JSON and generated C++ embed paths for the extraction host, so their whole-file SHA256 values can differ. Compare the bounded input and expected-output hashes recorded in the proof, and the test outcomes, rather than expecting path-bearing regenerated files to have identical bytes. Publishing or applying a live replacement was not admitted by this evidence.
