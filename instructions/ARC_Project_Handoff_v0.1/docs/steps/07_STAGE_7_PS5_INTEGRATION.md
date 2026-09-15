# Step 07 — Stage 7: PS5 integration

## Goal

Embed ARC as the host resource policy layer of a PS5 emulator.

## Integration order

1. guest executable boot path;
2. guest memory model;
3. CPU execution/JIT;
4. GPU command decode;
5. shader translation;
6. host resource creation;
7. presentation;
8. ARC observation;
9. ARC residency control;
10. ARC texture adaptation;
11. predictive streaming;
12. global governor.

## Architectural rule

Do not let ARC-specific assumptions leak into guest correctness code.

Use:

```text
guest semantics
→ neutral emulator representation
→ ARC policy
→ host representation
```

## Why this matters

If ARC is disabled, the emulator should still have a valid normal execution path.

## Performance objective

The emulator should minimize overhead through:

- JIT/recompilation rather than broad interpretation;
- host-native synchronization where valid;
- shader/pipeline caching;
- asynchronous work;
- minimal copy paths;
- ARC-managed residency.

## First emulator milestone

Do not begin with a blockbuster title.

Use a minimal/simple title or controlled homebrew-like workload where legal/available, validate:

- boot;
- render;
- input;
- timing;
- stable frame output;
- resource accounting.
