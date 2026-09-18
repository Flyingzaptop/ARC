# ARC Stage 14.5 вЂ” Wicked Engine Truth Test

- ARC source: be216d5847744e2d2aea059feecf5354d2d75467
- Wicked Engine: turanszkij/WickedEngine @ 0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7
- Verdict: **PASS**
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
- Physical multi-domain quality: True
- Online effect learning: True
- Performance win: True
- Bounded churn: True
- Full quality recovered: True
- Backend clean: True

## Mega C / Stage 15
- Verdict: **FAIL**
- Observer-only inference: True
- Truth labels supplied to inference: **NO**
- Truth labels supplied to controller: **NO**
- Resource semantic coverage: 40.31%
- Resource mean confidence: 0.837
- Scene retrieval accuracy: 80%
- Same-scene threshold recall: 100%
- Positive identity margin: 60%
- Mean identity margin: 0.0059
- Cluster recurrence: 100%
- Discovered clusters: 1
