# ARC 2.0 independent DX12 test matrix

Status: 2026-10-01. Frozen run manifests retain their actual repository HEAD (014781d or later diagnostic-only 5e03b90); the measured frontend source remains `014781d`. The frozen cost matrix uses frontend DLL SHA-256 `e8b3917c5285343a55eaa13df8eac850db50c3cbc4bf52533a5f3415731b84b6`. Each of two rounds used A-B-C-D-D-C-B-A, 10 seconds warmup and 12 seconds measured. A is native, B passthrough, C observe and D optimize. Values are medians of successful application Present-return cadence in Hz, **not display FPS**.

| Pinned codebase and workload | A | B | C | D | Verdict |
| --- | ---: | ---: | ---: | ---: | --- |
| WickedEngine `df44c3db4c4927492bc9c791eac715d98d7ed091`, 65k Instances | 37.07* | 37.69* | 34.02* | 34.80* | **INVALID**: four arms had zero measured Presents; one native bracket drifted +23.52% |
| WickedEngine, moving-camera Shadows | 143.78* | 132.62* | 103.07* | 96.52* | **INVALID**: three arms lacked independent measured rows |
| FidelityFX SDK/Cauldron v1.1.4, Brixelizer GI static Toyshop | 59.99 | 20.09 | 18.89 | 19.08 | **UNSUPPORTED** IID transparency; negative diagnostic D −68.19%; B quality unresolved |
| FidelityFX SDK/Cauldron v1.1.4, CACAO Sponza | 59.99 | 36.87 | 35.29 | 36.19 | **UNSUPPORTED** IID transparency; negative diagnostic D −39.67%; B quality unresolved |
| Microsoft MiniEngine `e5975f9b0744fc5096dd593ed72bf8f5c004164f`, Sponza | 144.03 | 144.03 | 144.03 | 144.02 | **NEUTRAL** cadence under 144 Hz cap; CPU gate unproven |
| MiniEngine, CesiumMan glTF | 144.03 | 144.02 | 144.00 | 144.03 | **NEUTRAL** cadence under 144 Hz cap; CPU gate unproven |
| DiligentEngine `e24e719488a2127b28878c3450d49cb76063c391`, DamagedHelmet PBR | 1783.61 | 1801.94 | 1741.38 | 1744.98 | **INVALID**: native bracket drift −22.77% and −29.58% |
| DiligentEngine, Tutorial14 fixed-step compute | 1477.76 | 1482.47 | 1430.14 | 1518.03 | **INVALID**: native bracket drift −56.91% and −56.22% |
| bgfx `b743ef8f7e875b3d89105f48f93a516b0631a0a3`, 17-drawstress at 8,000 draws/frame | 335.75 | 34.29 | 33.38 | 32.50 | **UNSUPPORTED** IID transparency and failed quality; negative diagnostic D −90.34% |
| bgfx, 21-deferred | 144.03 | 143.78 | 141.52 | 141.90 | **UNSUPPORTED** IID transparency; 144 Hz cap; B quality unverified |

`*` Wicked numbers are raw **unpaired** medians from measured arms only. Their summaries deliberately leave `mode_cadence_median_hz` empty because complete measured brackets do not exist. Instances arms 5 D, 10 B, 13 D and 16 A had zero successful measured Presents. Shadows arm 3 C had frontend rows but no native recorder rows; arms 5 D and 7 B had zero measured Presents. The second Instances native endpoint is absent. These incomplete sequences support no throughput claim.

All ten frozen matrices have raw archives in `docs/evidence/arc2-20260930/`. The archive names are the workload IDs below suffixed `-matrix-v2.zip`, except Brixelizer uses `cauldron-brix-matrix-v2.zip`:

| Workload ID | Archive |
| --- | --- |
| `wicked-instances-65k` | `wicked-instances-65k-matrix-v2.zip` |
| `wicked-visibility-shadows` | `wicked-visibility-shadows-matrix-v2.zip` |
| `cauldron-brixelizer-gi` | `cauldron-brix-matrix-v2.zip` |
| `cauldron-cacao` | `cauldron-cacao-matrix-v2.zip` |
| `miniengine-sponza` | `miniengine-sponza-matrix-v2.zip` |
| `miniengine-cesiumman` | `miniengine-cesiumman-matrix-v2.zip` |
| `diligent-helmet` | `diligent-helmet-matrix-v2.zip` |
| `diligent-compute` | `diligent-compute-matrix-v2.zip` |
| `bgfx-drawstress-8000` | `bgfx-drawstress-8000-matrix-v2.zip` |
| `bgfx-deferred` | `bgfx-deferred-matrix-v2.zip` |

The extra `diligent-compute-realtime-matrix-v2.zip` is version-separated from fixed-step Tutorial14. Its A/B/C/D medians are 1793.24/1640.41/1616.08/1704.29 Hz, bracket-adjusted D −5.55%, and native endpoint drift +5.27%/+1.84%. It must not be pooled with fixed-step rows. Earlier `324081d` Diligent cost-baseline archives are also separate from `e8b3917c`: Helmet D was −39.01%, Tutorial14 D −82.17% before the IR history eviction fix.

## Final frontend compatibility smoke

The later source commit `02a6d33` produced DLL SHA-256 `c7585b01eccc108867a45104b779055d6c19d1ffcf2dcf5cf04cb62dee0ef215`. One fresh 10-second passthrough run per codebase exited 0 by window close, wrote a fresh IR dump and recorded frontend Presents:

| Workload | Frontend Presents | Run directory below `build/arc2-testbeds/runs/` |
| --- | ---: | --- |
| Wicked Instances | 318 | `final-c758-wicked-instances-65k` |
| Cauldron Brixelizer GI | 167 | `final-c758-cauldron-brixelizer-gi` |
| MiniEngine Sponza | 1292 | `final-c758-miniengine-sponza` |
| Diligent DamagedHelmet | 17353 | `final-c758-diligent-helmet` |
| bgfx drawstress | 317 | `final-c758-bgfx-drawstress-8000` |

`validate-command-payload.py` passed with `--require-kind` for every kind actually retained in each fresh IR. Wicked, Cauldron and MiniEngine retained draw, indexed draw, dispatch and indirect; Diligent retained draw and indexed draw; bgfx retained indexed draw. Each history reached the 65,536-record cap, so this proves typed arguments and retained IA/RS state only, not complete command coverage, replay correctness, GPU effects or image quality. These smokes are not new performance trials and must not be pooled with the frozen matrix.

The five fresh run manifests, complete IR dumps, both Present CSVs where
available, and payload validation JSONs are archived in
`docs/evidence/arc2-20260930/final-c758-smokes.zip`.

## Independent image evidence and limits

Wicked Instances' final fixed-step 25-frame A-B-A PNG sequence was byte-exact. Wicked Shadows' 25-frame sequence remains inconclusive: some paired local/global errors exceed native drift; temporal equivalence is not admitted. Consult its archived per-frame A-B and A-A metrics. MiniEngine Sponza scene ROI stayed within native A-A drift; CesiumMan scene ROI was pixel-exact in fixed-step A-B-A. The variable MiniEngine HUD prevents a full-frame exactness claim. Diligent Helmet's 30 PNGs were byte-identical in A3/B3; fixed-step Compute's 30 A-B frames stayed within A-A native drift. These comparisons concern the captured passthrough binaries, not the later final DLL.

The bgfx drawstress final 25-frame A-B-A capture **fails** image equivalence: scene-crop mean absolute channel error averaged 34.36 in A-B versus 4.28 in A-A, about 8.0 times native drift. All 75 raw TGAs, 75 converted PNGs, three run manifests and full metrics are archived in `docs/evidence/arc2-20260930/bgfx-drawstress-final-quality-raw.zip`. Cauldron has native Toyshop and Sponza screenshots but no aligned A-B-A image comparison; its B quality is unverified. bgfx deferred B quality is likewise unverified.

Inspected passthrough IR has known `QueryInterface` gaps: bgfx probes `ID3D12Device6` through `ID3D12Device14`, Cauldron probes `ID3D12Device8` and `ID3D12Device10`. The apps continued, but these requests returned different HRESULTs from native. Inspected MiniEngine and Diligent runs had no such IID gap. CPU processing-to-submission cost, GPU frame span and display FPS were not independently measured.

All 44 archived Optimize-arm IR summaries across the ten frozen matrices and
the separate realtime diagnostic report `accepted_actions: 0`. Thus these
cost results measure the generic capture and analysis path; they demonstrate
no successful external rewrite or optimization speedup.

## Reproduction and source boundaries

Pinned source trees, instrumented binaries and local runs are under ignored `build/arc2-testbeds`; source hashes, acquisition and build commands are in `docs/evidence/arc2-20260930/testbeds.json`. Source scripts in `scripts/arc2/` bootstrap generic D3D12/DXGI interception, select deterministic public workloads, expose fixed simulation time where needed, capture native Presents or GPU readbacks, and bypass missing ATL on this host. They contain no engine-specific semantic optimizer hints. The Cauldron release archive SHA-256 is `0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952`.

Select an ID from `scripts/arc2/workloads.json` and run `scripts/arc2/run-counterbalanced.ps1` with `-Manifest`, `-WorkloadId`, `-OutputRoot`, `-Rounds 2`, `-WarmupSeconds 10`, `-Seconds 22`. `run-testbed.ps1` requires a fresh directory and records executable/frontend hashes, exit path, raw QPC rows and environment. `summarize-counterbalanced.py <matrix.json> --output <summary.json>` validates order and windows; `package-matrix.py <matrix-dir> <fresh-archive.zip>` packages raw rows and IR provenance. For the two incomplete Wicked matrices, use the packager's `--allow-invalid-arms` option so a missing native CSV remains absent in the archive. Screenshot runs are separate from timing runs. No external engine rewrite was admitted to Optimize.
