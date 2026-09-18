# ARC Stage 14.5 вЂ” Wicked Engine Truth Test

- ARC source: bb75dd9684d81c1ee041886a56061d74c69b2c0a
- Wicked Engine: turanszkij/WickedEngine @ 0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7
- Verdict: **FAIL**
- Native target: **1920x1080**
- Temporal / FSR / FSR2: **OFF**
- Timing: **Wicked's D3D12 GPU timestamp profiler**
- Controller scene labels: **NOT USED**

## Gates
- Exact pinned sources: True / True
- Complex renderer observed: True
- Scene coverage: True
- ResourceGraph clean: True
- Observation/control clean: True
- Physical multi-domain quality: False
- Online effect learning: False
- Performance win: False
- Bounded churn: True
- Full quality recovered: True
- Backend clean: True

## Mega C / Stage 15
- Verdict: **FAIL**
- Observer-only inference: True
- Explicit truth isolation verified (inference): True
- Explicit truth isolation verified (controller): True
- Resource semantic coverage: 78.74%
- Resource mean heuristic score (uncalibrated): 0.821
- Scene retrieval accuracy: 100%
- Same-scene threshold recall: 100%
- Positive identity margin: 100%
- Mean identity margin: 0.082
- Cluster recurrence: 80%
- Discovered clusters: 2
