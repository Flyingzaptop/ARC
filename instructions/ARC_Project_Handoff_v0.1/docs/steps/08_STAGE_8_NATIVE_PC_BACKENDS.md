# Step 08 — Stage 8: Native PC-game integration

## Goal

Apply ARC to native games after the core proves itself in controlled environments.

## D3D12

Use an interception/proxy/wrapper strategy that can observe API objects and calls with minimal overhead.

The exact deployment mechanism may differ by game and protection model.

## Vulkan

Use an explicit Vulkan layer where possible.

## Steam/Epic/local executable

The launcher is not the important boundary.

ARC attaches to the graphics API process path.

Thus:

```text
Steam
Epic
local EXE
other launcher
```

are just different ways the game process begins.

## Protected games

If anti-cheat or protection disallows ARC:

- do not evade it;
- mark unsupported;
- seek explicit integration.

## Compatibility database

Long-term maintain:

```text
game
version
API
ARC features safe
ARC features disabled
known issues
```

But aim for automated safety classification so per-game profiles remain exceptions, not the main architecture.
