# Implementation continuation notes (not completion claims)

Updated 21 Sep2026, approximately21:00UTC. Active work in ARC-perceptual,
branch codex_den/generic-dx12-runtime. No agents/commercial games. Deadline
22Sep04:30Warsaw (02:30UTC). Observed loaded GPU remains115W despite requested30W;
keep actual power cohorts separate and never label these as30W results.

## Implemented since daec4d5

- ABI CONTRACT_12 /144-byte ControlValue: compact full-tile PSO, per-recipe
  256-bin empirical error tables, >=3distinct candidate frames plus neutral noise,
 8-observation hysteresis and immediate original fallback for unknown inputs.
- Sparse raw-buffer shadow probes on identical inputs, <=1/16tiles, >=31frame
 spacing, one in-flight/device,16MiB pair. Per-output masks/64bit epochs, fence
 readback, maximum normalized error across outputs. App outputs untouched.
-8control slots per device pool; additional devices fail closed to keep combined
 spatial/scratch default heaps below256MiB. Allocation counters include scratch.
- Dynamic4x rates and half-step mips through+4; group-coherent shared-memory path
 with convergent acyclic barriers and bounded local pointers; group-uniform
 original fallback on incomplete coarse regions. No shared spatial probes yet.
- One fixed-count independent normalized sample-mean loop may use75/50/25percent.
 Recurrent, dynamic, unnormalized and RayQuery loops are not eligible. Neutral and
 rollback preserve original arithmetic. Sample reduction currently global/pass,
 not combined with the spatial actuator.
- Quality worker scheduling priority and overlapping renewal timing only under
 still-valid incumbent proof. Critic CPU memory remains~600MiB (512target unmet).
- Compiler child now owned by kill-on-close Job Object.

## Evidence

78 CTests passed (before final group/sample additions); compact learned-map GPU
oracle passed, including immediate restoration on new detail. Sparse raw probe
GPU tests passed (multiple outputs, partial tiles, mask mismatch, stale epochs,
untouched app outputs). Native shader fixture perf-samples-native-13 passed all
pixel CPU oracles including shared full/partial groups and normalized samples.
Actual formerly-rejected shader367 now assembles/validates; this is NOT a speed
or quality result. Raw source IR stays outside the repository/package.

perf-learned-spatial-07 had no accepted effects: image probe map warmup was too
short.40frame warmup fixes it. perf-learned-spatial-08 has live quality passes but
no useful timing gains. B timing warmup now40frames as well; needs fresh rerun.
Previous expanded-05 local gain is not a new full-session performance result.

## Still required

- Performance proposal cache keyed by device/driver/render/profile, fresh proof.
- Further measured overhead reduction; no repeatable new full-session gain yet.
- Verify phase-locked host/init bounds and holdout route; add independent object/
 light/CPU-command dynamics if feasible. Same host for O/N/V/B/A.
- Final three-order15run matrix,25matchedframes/route, Wicked2x60s, overlay, package,
 full tests, git/push and honest report by deadline. Preserve negative results.

