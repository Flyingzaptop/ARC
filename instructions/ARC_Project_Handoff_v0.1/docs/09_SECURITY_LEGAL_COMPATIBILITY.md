# 09 — Security, legal and compatibility boundaries

## ARC should not rely on circumvention

Do not design core functionality around:

- DRM bypass;
- anti-cheat bypass;
- kernel protection bypass;
- driver-signing bypass;
- hidden process injection intended to evade detection.

This is both a compatibility and engineering decision.

## Protected games

If a protected game rejects interception:

- mark it unsupported;
- provide an official integration path if one becomes available;
- prefer engine/plugin integration where possible.

Do not turn ARC into an evasion project.

## Emulation content

The emulator/backend design should assume user-owned/legal content workflows and focus on execution/optimization architecture.

## Driver research

Kernel experimentation must use normal OS development/testing mechanisms. It should remain isolated from production ARC until it has a clear benefit that user-space cannot provide.

## Crash isolation

ARC should:

- validate pointers/handles;
- avoid blocking render threads;
- use bounded buffers;
- detect event overflow;
- disable optimization after consistency failure;
- keep trace files crash-recoverable where possible.
