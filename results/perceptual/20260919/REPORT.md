# Perceptual runtime delivery — 2026-09-19

## Outcome

| Work | Result | Scope |
| --- | --- | --- |
| Mega F / 21+22 | Implemented; native and independent validation PASS | Reversible SRV trials, independent pixel/timing guards, reliable rollback |
| Mega G / 23+24 | Implemented; deterministic and combined native validation PASS | Predictive restoration, bounded gain calibration, cooldown, quarantine, versioned checkpoints |
| Independent renderer | PASS | Same core on native compute and Microsoft's raster renderer; no renderer-specific policy branch |
| Third-party DX12 connection | Read-only prototype exercised in FPV.SkyDive | Real game API calls observed; game optimization is **not enabled** |

This is not a claim that arbitrary games can already be optimized. The generic
process probe currently supplies partial API counts, not a complete live resource
graph or independently comparable game reference frames. Unsupported mutation and
reference capabilities keep the portable policy in observe-only mode.

## Verified F/G behavior

Runtime source for final native/portability captures:
`4e5b0dde1d0a91d52b7246f9c4384d5387cd3f49`.
The final real-game diagnostic probe adds only failure context at `af44bbf`.

- Native DX12 SRV mip change: baseline median 0.578560 ms, modified 0.091136 ms,
  restored baseline 0.577536 ms. Conservative gain: **0.486400 ms**.
- Positive image mean error: 0.0000003201; peak error: 0.0002691.
- A faster but damaged mip produced peak error about 0.5629 and was rejected.
- Growing GPU-rendered coverage reached 2.4414%; the eight-frame prediction was
  3.0654%. The original SRV was restored before current coverage reached the 3% guard.
- Learning updated only after independently accepted trials. Tests verify quarantine,
  bounded storage, context isolation, schema rejection, cooldown and fault recovery.
- Reproduced and fixed two edge cases: extreme finite timing values overflowing an
  even-sized median, and stale baseline generation allowing a temporary probe before
  eventual rejection. Both now have regression tests.

These are controlled per-dispatch measurements, not game FPS gains. Pixel guards
are not a calibrated claim of human-perception equivalence. The synchronous probe
contract needs a suitable slow path and reproducible state; it is not a Present hook.

## Portability

Microsoft D3D12HelloTexture at `213dd4fd4918ea009dd8f35adee1aff1f2ecaba4`
uses its original shader, geometry and 1280x720 rendering. The overlay supplies a
normal mip chain, DX12 SRV changes, timestamps and backbuffer readback.

Both renderer executables link the same Release `arc-core.lib`, SHA-256:
`6EF2A1CFB78E7CBB82A555E37E00A2D77A1978EED86B20A92363FE933D233652`.
The raster trial preserved the image but saved only 0.001024 ms, below the unchanged
0.02 ms guard. The core correctly rejected retention and restored the original SRV.
The separate validator recomputed the reason from raw pixels/ticks.

## Real-game connection

FPV.SkyDive 2.9.1b ran through Steam on DX12. The engine-neutral DLL was attached to
the explicit game PID. Solo freestyle / Abandoned Factory was loaded through the
normal UI. No game files, engine APIs or security settings were changed.

The first 238-second observation window recorded one native Present error
`0x887a0001`. The game continued running, but that window remains **FAIL** under a
strict zero-error gate. Its cause is unresolved; it must not be blamed on the game
or declared fixed without evidence.

An uninstrumented control did not reproduce the error. A subsequent instrumented
window lasted **346.68 seconds** and recorded:

- 47,056 Present calls;
- 188,096 queue submissions;
- 59,670,413 indexed-draw calls;
- zero hook failures and zero Present failures;
- zero quality mutations.

Windowed/fullscreen transitions were exercised both without and with the probe.
The later window is PASS only for that observation scope. This does not erase the
earlier failure or establish universal compatibility. No game speedup is claimed.

Steam initially required manual sign-in; the user completed it. Authentication
and game-protection code were not automated or bypassed. Full Steam/player logs
are kept locally; public evidence contains only curated non-account diagnostics.

## Tests and retained negative evidence

- Final local Release: **65/65**, including 20 GPU tests.
- Debug non-GPU/core/stress: **45/45**.
- Additional native hook-forwarding test: PASS. A deliberately invalid Present
  returned the identical native HRESULT before/after attachment; sync interval,
  error and healthy device status were faithfully recorded. This checks diagnostics,
  not the cause of the earlier game error.
- Independent raw-image/timestamp/forecast and external-renderer validators: PASS.
- [Hosted CI at the tested F/G/portability source](https://github.com/Flyingzaptop/ARC/actions/runs/35434832553): PASS.
- Windows CI is extended to compile the optional generic observer and launcher.

The first full local run was 63/64: an unchanged legacy Mega B smoke missed its
lighting-domain requirement after a 0.003 ms short calibration estimate. Its isolated
rerun and the final full suite passed. Both outcomes are retained; no gate was lowered.
Early portability startup/timer-resolution failures and the first real-game Present
failure are also retained.

[Raw evidence archive](https://github.com/Flyingzaptop/ARC/tree/results/perceptual-20260919-verified/results/perceptual/20260919).
Archive attributes disable line-ending conversion; checksums are verified against
actual Git blobs, including the raw float32 images. The earlier archive branch is
retained but superseded for transport integrity.

## Remaining product work

1. Feed a bounded, lifetime-correct live work/resource graph from generic interception
   into F/G. Counters alone are insufficient.
2. Establish safe engine-independent reference/counterfactual capture for real games;
   reject motion/history ambiguity rather than inventing comparable frames.
3. Admit generic reversible game-resource actions only when those capabilities and
   identities are proven; the current game probe deliberately cannot mutate quality.
4. Expand compatibility coverage and diagnose the unresolved Present invalid-call.
5. Stage 15's **81/128** complexity gate remains deferred FAIL. Created-resource counts
   in a game do not prove the required unique-used-resource cohort.
