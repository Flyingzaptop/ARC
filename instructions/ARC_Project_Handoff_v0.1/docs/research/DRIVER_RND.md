# Driver R&D track

## Question

Should ARC eventually move below API interception and into a graphics driver?

Potential advantage:

- deeper residency knowledge;
- tighter scheduling integration;
- lower interception overhead;
- unified implementation;
- more direct access to memory/queue decisions.

Potential disadvantage:

- enormous hardware-specific scope;
- Windows WDDM complexity;
- firmware dependencies;
- display/power management complexity;
- certification/signing burden;
- vendor IP/closed components;
- risk of delaying the actual optimizer for years.

## Decision for v0.x

Do not block ARC on a custom Windows display driver.

## Windows research

Map which desired capabilities are unavailable in user-space:

- exact physical residency visibility;
- scheduling control;
- memory paging decisions;
- copy path control;
- queue prioritization;
- hardware telemetry.

Only implement a kernel/driver component if there is a specific measured limitation.

## AMD/Linux research

Most promising open laboratory:

```text
ARC research fork
     ↓
Mesa RADV
     ↓
amdgpu
     ↓
AMD RDNA GPU
```

This allows studying a real open Vulkan user-space driver plus open kernel-side components.

Potential experiment:

- integrate ARC resource scoring into allocation/residency decisions;
- compare API-level ARC vs driver-level ARC;
- measure overhead and behavior.

## NVIDIA/Linux research

NVIDIA publishes open GPU kernel modules, but the complete stack still relies on NVIDIA user-space and firmware components.

Use as architectural reference, not as an assumption that a complete open Windows NVIDIA driver is available.

## Why not build an NVIDIA Windows driver from scratch first

Because the project would become:

> "write and maintain a modern vendor GPU driver"

instead of:

> "build a universal optimization runtime."

That is a different multi-year project.

## Possible eventual architecture

```text
ARC Core
│
├── User-space API mode
└── Driver-assisted mode
      ├── vendor-specific telemetry
      ├── residency hints
      └── scheduling/memory integration
```

Driver assistance should be optional and capability-based.
