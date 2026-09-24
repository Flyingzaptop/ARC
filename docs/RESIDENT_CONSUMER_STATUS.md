# Resident draw consumer — completed bounded experiment

Base c7711e9; source-assisted Wicked, not universal ARC. User contract is archived
in experiments/wicked-resident-consumer/task-contract.txt.

Terminal result: positive CPU submission cadence at observed hardware settings.
A mean 19.3966 ms; B 19.7393 ms; C 11.3011 ms. Full GPU/display-completion gate
is not claimed. NVML active power ~62-65 W; 30 W is not verified.

Implemented visible IDs + resident instance data -> exact keys -> GPU sort ->
GPU pointer stream -> existing draw consumer. CPU scene/ECS/matrices/AABBs/visibility
remain original. Two main passes skip their build/sort/packing loops. Zero algorithm
readback and no new CPU waits/frames-in-flight. No PID/automatic extractor added.

Final oracle: 27,313,792 exact records, 418 same-frame primitive-ID/depth comparisons,
zero mismatches. Original EXE restored by matrix script. Default mode remains off.
Failed attempts and their fixes are retained in raw evidence.

See [report](reports/2026-09-24-resident-draw-consumer.md) and
[evidence](evidence/resident-consumer-20260924/manifest.json).

Remaining: validated portability/general admission, actual completed/display cadence
measurement, and recheck under a confirmed 30 W cap if required. No further queue
expansion is justified by this result alone.
