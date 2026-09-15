# 05 — PS5 backend concept

## 1. Why PS5 is a strong first high-control client

A native Windows game owns its renderer and only exposes API behavior.

A PS5 emulator already needs a translation layer between guest semantics and host graphics APIs.

That creates an ideal insertion point:

```text
PS5 game
   ↓
guest GPU / OS semantics
   ↓
PS5 emulator GPU frontend
   ↓
ARC virtual resource layer
   ↓
host D3D12/Vulkan backend
   ↓
vendor driver
```

ARC can therefore reason about a resource **before** deciding the exact physical host representation.

## 2. User experience target

```text
Insert/read legally available game media or select supported game image/file
→ emulator identifies title
→ ARC loads/calibrates hardware profile
→ Play
```

No exposed manual quality knobs are required for normal use.

## 3. Key advantage: memory virtualization

The guest may believe that a logical texture has a complete mip hierarchy while host residency keeps only the mips currently valuable.

Concept:

```text
Guest texture: 4096² logical

Host:
mip0 4096²  not resident
mip1 2048²  resident
mip2 1024²  resident
...
```

This requires host API support and careful shader/descriptor translation. It cannot be assumed safe for every guest resource.

## 4. Resource classification in emulator mode

The emulator can potentially know more than a generic Windows interceptor:

- guest usage flags;
- guest memory region;
- texture format;
- sampler state;
- shader resource access patterns;
- guest queue intent;
- guest barriers;
- guest resource lifetime.

That information should flow into ARC instead of being discarded.

## 5. PS5-specific priorities

Initial emulator backend priorities:

1. correct guest CPU execution;
2. correct guest memory model;
3. correct GPU command translation;
4. shader translation;
5. stable frame pacing;
6. ARC observer integration;
7. ARC residency;
8. texture residency adaptation;
9. predictive streaming;
10. temporal optimization.

ARC must not become an excuse to hide emulator correctness bugs.

## 6. Low-VRAM strategy

Order of preference:

1. remove speculative residency;
2. keep cold resources in RAM;
3. partial texture residency where safe;
4. drop unnecessary high mips where safe;
5. prefetch based on guest usage;
6. only later manipulate broader render-quality resources.

## 7. Separation from game acquisition

The project should focus on runtime/emulation and user-owned/legal content workflows. DRM or anti-circumvention bypass is not part of ARC.
