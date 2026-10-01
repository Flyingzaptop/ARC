# ARC2 experimental frontend package

Windows x64, native D3D12/DXGI, compatible GPU and Microsoft Visual C++ runtime
are required. This is a source-bootstrap developer package, not an installer or
a transparent injector for arbitrary games. Commercial games were not tested.
Interface coverage and CPU cost gates remain open; read ARC2_RESULTS.md first.

Run the included owned fixture from this directory in PowerShell:

```powershell
Remove-Item Env:ARC1_DLL -ErrorAction SilentlyContinue
$env:ARC2_MODE = 'passthrough'
$env:ARC2_DLL = (Resolve-Path ./arc2-frontend.dll).Path
$env:ARC2_OUTPUT = Join-Path $PWD 'fixture.ir.json'
./arc2-binding-native.exe
```

For a real window/resize/readback test:

```powershell
./arc2-native-lab.exe "$PWD/window-test" 16 24 debug
```

The optional `debug` argument requires the Windows D3D12 debug layer. Omit it
where unavailable and record that debug validation was not performed. The fixture
writes GPU readback, frame timing and diagnostic evidence alongside its prefix.
To use the native control, remove `ARC2_MODE`; do not substitute a missing DLL.
To test the exact duplicate-clear rule, set `ARC2_MODE=optimize`. This rule is not
a general game graphics optimizer or proof of useful acceleration.

`arc2-frontend-meter.dll` is a separate intrusive CPU diagnostic build. Use it only
for attribution, never as the DLL in the normal A/B performance comparison.
It emits `.cpu.json` through explicit `Arc2Dump`/bootstrap `flush()` while the
host is alive. The default DLL contains no such per-call CPU meter.

Testbed source uses the included generic creation bootstrap only. Full source,
five-codebase harness patches, pins, raw measurements and image evidence are on
the repository's `arc2/runtime-ir` branch. The package manifest records exact
file hashes and distinguishes the measured DLL revision from packaging history.
