# Shadow reuse prototype and multiple independent lighting/ray loops

Status: partial implementation. This does not complete universal shadow caching, lighting/reflections or arbitrary inline RT. The live DLL does NOT skip shadow-map construction. No new Cauldron/FPS claim is made.

## Implemented

- Engine-independent exact-input shadow reuse decision core in include/arc/shadow_reuse.hpp. It uses full byte equality, resource generation/write epoch, queue/fence identity and completed fence. Unknown input coverage, incomplete pass, unpreserved output or unknown queue exclusivity forbids reuse. Input records limited to 1 MiB / 64 entries. Resource ownership, complete canonical input snapshots and whole-pass execution remain backend responsibilities.
- Native D3D12 prototype renders an actual triangle into a D32 shadow depth texture. Eight frames include stationary, moved and returned light-space positions. It performs three original clear/draw passes and five cache reuses. Every frame is compared byte-for-byte with a separate freshly rendered depth texture; movement demonstrably changes the depth image. This is avoided work count, not a measured 62.5% session GPU-time gain.
- Unit checks cover changed bytes at the same address, output writes, resource generation changes, queue/fence changes, pending GPU work, incomplete inputs/pass, invalidation and memory capacity. No approximate motion heuristic permits a hit.
- Removed one-loop-per-shader limitation for independently proved sample/ray means. Both disjoint loops receive separate counts and normalization with the existing shared percentage control. Tests use two texture loops (8/16 original samples) and two inline ray sets (8/16 rays), including 1%, 37%, 50%, neutral and restoration.
- Allow pure loop-invariant values (e.g. shared ray origins) to escape a reduced loop: dependency analysis excludes induction variables, accumulators and ray state. At least one iteration remains. Full Proceed loops are unchanged. Dependent rays are still rejected.
- Fixed SSA-name matching to compare whole identifiers instead of substrings when checking escaping ray handles.
- Fixed pixel-control metadata allocation to count distinct metadata nodes. A new two-loop shader previously collided with an existing loop-metadata ID; the GPU oracle now passes.

## Validation

Release worker/probe/native builds passed. arc-shadow-reuse-tests and arc-target-feedback-tests passed. Pixel native oracle with live DLL passed, including shadow prototype, two texture loops, mips, PCF and live rollback. Compute native oracle with live DLL passed, including two inline ray sets and existing dependency rejections. Multi-loop comparisons are direct transformed-shader GPU oracles; existing live-injection cases additionally validate the runtime path, not universal multi-loop coverage in commercial games.

Evidence directories:
- C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/shadow-multi-live-pixel-01
- C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/shadow-multi-live-ray-01
- C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/multi-ray-native-04 (dual-ray original/transformed IR)

## Remaining integration

The current live command mirror records in place; its record helper does not retain a replayable stream. A live shadow adapter still needs complete input-content tracking including mapped upload/GPU writes, delayed whole-pass command replay, clear/draw substitution as one operation, output preservation and lifetime/fence handling. The standalone fixture supplies complete evidence; the live DLL cannot currently supply it and therefore must not enable this cache.

Lighting/reflection support still requires proved shader patterns, not effect names. Dynamic loop bounds, arbitrary weighted filters, bindless/unsupported layouts, arbitrary ray candidate handlers and full DXR path tracing remain outside current coverage. No 100% claim is justified.
