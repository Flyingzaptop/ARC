# Mega F — portable perceptual trials

Stages 21/22 implement a portable capability contract, independent image critic
and reversible single-action transactions. No engine headers, object names or
scene labels enter `perceptual_trial.hpp` or the critic.

## Evidence and admission

Stage 19 importance/confidence and measured GPU cost admit and prioritize
candidates. Expected gain is a proposal, never acceptance evidence. A host must
explicitly support the mechanism, target generation, synchronization, original-state
restoration and reproducible reference capture. Unsupported games/actions abstain.

The critic compares full/modified/full **linear normalized RGB** images from the
same reproducible input state and resource generation. It checks reference drift,
mean/peak changed pixels and worst 8x8 tile error. It independently checks median
GPU durations, before/after baseline stability, absolute and fractional gain.
Raw pixels and timestamp evidence are retained by the native fixture, and a separate
Python program recomputes the result without trusting the controller's metrics.

These are conservative pixel safeguards, not calibrated human-perception judgments.
HDR, temporal reprojection, motion matching and nonrepeatable references are not
supported by this version. A missing/rejected comparison cannot authorize retention.

## Transaction contract

`prepare` pins identity and saves original state without changing quality. `apply`
must verify the same target/generation and synchronize outstanding use. `restore`
is idempotent, restores the saved original (not a guessed inverse), and reports real
completion. Even a failed apply can have partial effects, so it requires restore.

The reference state is restored before evaluation and capture of the final full
reference. Only an independently accepted probe can reapply/retain the action.
Rollback failure latches a fault; recovery requires actual restoration through the
same host. A different host cannot clear it. `finish` releases temporary probe
state but must preserve the rollback snapshot while a retained action exists.
Host lifetime must cover retained actions; restore before destroying the controller.

This synchronous orchestration is for a serialized slow path. It must not be called
inside Present or an API hook: captures/readbacks can wait. A live adapter must
schedule it only where it can supply the stated evidence without freezing gameplay.

## Native validation

`arc-perceptual-native` uses native D3D12 compute work and changes only the
`MostDetailedMip`/`MipLevels` fields of a real shader resource view after GPU
completion. It does not call engine quality settings. Input is a mipmapped texture;
the workload is deliberately bandwidth-sensitive and contributes to a small region.
The fixture has a smooth acceptable lower mip and a deliberately damaged lower mip.

Actual readback coverage feeds Stage 18 and Stage 19; actual GPU samples supply
candidate cost. The fixture's constant-background visibility oracle is test-only,
not a claim that arbitrary-game visibility has been solved. A positive trial must
gain time and meet image guards. The faster but visibly damaged trial must be
rejected and the original SRV restored.

`scripts/validate-perceptual-native.py <run-directory>` checks raw float32 RGB,
all timestamp conversions, physical mip identities and image restoration. Benefits
here are per-dispatch fixture results, not game FPS or a universal speedup.

Mega G, independent-renderer portability and third-party integration remain
separate steps. This fixture alone does not close those steps.
