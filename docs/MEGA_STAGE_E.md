# Mega E — temporal visibility and visual importance

Stages **18 + 19**. Mega E consumes the conservative attribution/screen-region
foundation from Mega D and remains **observer-only**. It does not change graphics
quality, residency, mips, shadows, LOD, or renderer state.

## Goal

Mega E answers two new questions:

1. **Stage 18 — Visibility / Occlusion / Temporal Relevance**
   - Is a tracked contribution visible now?
   - Is it entering or leaving the visible image?
   - Was it visible recently?
   - Is its visible contribution likely to rise in the next few frames?

2. **Stage 19 — Visual Importance**
   - Given the best available visibility evidence, how cautious should ARC be
     about degrading this contribution?
   - How do screen coverage, centrality, future relevance, composition evidence
     and uncertainty combine into a bounded importance estimate?

GPU cost is deliberately **not** part of Visual Importance. Cost belongs to
Stage 20 and later optimization policy. Keeping cost separate prevents an
expensive object from being declared visually important merely because it is
expensive.

## Stage 18 core

Files:

- `include/arc/temporal_visibility.hpp`
- `src/core/temporal_visibility.cpp`
- `tests/temporal_visibility_tests.cpp`

`TemporalVisibilityModel` tracks stable visual work across frames and exports:

- estimated visible coverage;
- conservative Stage 17 coverage upper bound;
- coverage velocity;
- predictions at 2, 8 and 30 frames;
- temporal relevance;
- confidence;
- frames since observation / visibility;
- visibility phase:
  - Unknown
  - Hidden
  - Visible
  - Entering
  - Leaving

Missing observations decay gradually. One dropped sample does not instantly make
a previously visible contribution unimportant. Sustained absence does decay.

### Evidence hierarchy

Strong evidence:

- sampled visible coverage, such as a future occlusion-query path;
- known non-reachability to the presented frame.

Weak evidence:

- Mega D local raster coverage upper bound.

Native D3D12 validation also covers the strong-evidence path. The
`arc-temporal-visibility-native` test renders a front draw and a fully depth-
occluded draw under real D3D12 occlusion queries, resolves samples-passed data,
normalizes it through `visible_coverage_from_occlusion()`, and feeds the result
into Stage 18. This proves the core can consume real GPU visibility evidence
without engine object/camera metadata.

A raster upper bound is never silently promoted to actual visibility. When it is
the only signal, confidence is reduced.

## Stable track identity

Temporal inference needs identity across frames.

`make_visual_track_fingerprint()` derives a session-local fingerprint only from
backend-neutral Stage D work structure:

- work kind;
- pipeline fingerprint when available;
- input and output resource dependency identity;
- draw/dispatch/copy work magnitude;
- raster target dimensions.

It does not use engine object names, scene labels, Unreal/Unity/Wicked semantics
or game-specific IDs.

The fingerprint explicitly carries confidence. It is not claimed to be a
universal engine object identity.

`make_potential_visibility_observation()` bridges an `AttributionNode` into
weak Stage 18 evidence without fabricating actual visibility.

## Stage 19 core

Files:

- `include/arc/visual_importance.hpp`
- `src/core/visual_importance.cpp`
- `tests/visual_importance_tests.cpp`

The model exports an interpretable decomposition:

- coverage component;
- visibility component;
- temporal component;
- screen centrality component;
- perceptual sensitivity component;
- composition component;
- uncertainty reserve;
- final importance score;
- confidence.

Unknown evidence is conservative:

- unknown centrality defaults to central;
- unknown perceptual sensitivity defaults to 1.0;
- unknown composition relevance defaults to 1.0;
- lower confidence adds an uncertainty reserve instead of making work look cheap.

This is intentional fail-safe behavior before perceptual actions exist.

## Current importance semantics

The current score is a **risk-of-visible-degradation estimate**, not a learned
human-perception probability.

It uses square-root screen-area scaling so small but real contributions are not
discarded too aggressively. Short-horizon predicted coverage contributes to
importance so an entering object can become important before reaching its peak
coverage.

The score is bounded to [0,1].

The current model does not claim to understand:

- faces, UI, text or semantic salience;
- contrast sensitivity;
- texture-frequency masking;
- motion masking;
- foveated eye tracking;
- subjective image quality.

Those can be added later without changing the separation between observer,
decision model and critic.

## Cross-stage scenario validation

`tests/mega_e_scenario_tests.cpp` exercises a synthetic camera-turn sequence.

Truth trajectories exist only in the test harness. Object names/labels do not
enter Stage 18 or 19 inference.

The scenario checks:

- an entering contribution gains relevance/importance;
- a leaving contribution loses importance;
- an occluded contribution stays low;
- a brief observation gap preserves recent relevance;
- sustained absence decays;
- all outputs remain bounded.

## Interactive visual debug milestone

Windows target:

`arc-mega-e-visual-fps`

Source:

`samples/mega_e_visual_fps.cpp`

This is intentionally a small first-person debug world rather than a game-engine
integration. It exists to make Stage 18/19 behavior visually inspectable.

Controls:

- W/S — forward/back
- A/D — strafe
- Shift — sprint
- Left/Right arrows — look
- hold right mouse button + move mouse — look
- Esc — exit

The overlay displays per tracked object:

- ARC track ID;
- current Visual Importance;
- confidence;
- visibility phase;
- estimated visible screen coverage;
- 8-frame predicted coverage.

The simulator computes simple screen projection and approximate occlusion as
evaluation/sample evidence. Object display names are used **only by the UI** and
never enter ARC inference.

The visual scene now uses world-space oriented boxes rather than camera-facing
billboards. Walls retain fixed orientation as the player moves and turns. The
debug world contains 18 simultaneous tracked objects, including multiple moving
objects with independent trajectories (lateral oscillation, depth patrol, orbit,
vertical bobbing and side motion). Motion changes world transforms but preserves
stable ARC track identity.

The compact overlay table shows every object at once and includes:

- static/moving marker;
- object name;
- visibility phase;
- current visible coverage;
- 8-frame predicted coverage;
- Visual Importance;
- confidence.

The colored ARC border is the debug overlay. It makes it easy to watch:

- a large central object become highly important;
- an object behind the player drop toward Hidden;
- an occluded object remain low despite large potential raster area;
- an object entering the field of view gain predicted relevance before peak size;
- importance decline smoothly instead of flickering to zero.

This sample is a visual-debug milestone, not proof that generic D3D12 occlusion
has already been solved for arbitrary games.

## Acceptance

Mega E is considered functionally complete when:

- Stage 18 deterministic tests pass;
- Stage 19 deterministic tests pass;
- combined scenario tests pass;
- Linux and Windows builds pass;
- existing ARC regression suite remains green;
- the Windows visual debugger builds;
- the native D3D12 attribution foundation smoke still passes on the user's GPU;
- the native Stage 18 D3D12 occlusion-query test passes on the user's GPU;
- the visual debugger exhibits stable, bounded importance behavior under movement
  and camera rotation.

No optimization/performance gain is required for Mega E because it remains
observer-only.

## Local validation

The one-command local path is:

`scripts/mega-e-local-check.ps1`

It configures a Release build with GPU tests, builds Stage 18/19 plus the visual
debugger, runs the deterministic Mega E tests, runs the native D3D12 attribution
foundation smoke, and launches the visual debugger.

## Boundary to Mega F

Mega E must not mutate quality.

Mega F (Stages 21 + 22) will consume:

- Stage 18 temporal visibility;
- Stage 19 Visual Importance;
- Stage 20 measured/estimated cost;

and introduce reversible perceptual actuators plus an independent visual-damage
critic.

The decision model must not grade its own action.
