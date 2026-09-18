# ARC Stage 14.5 вЂ” Wicked Engine Truth Test

- ARC source: 1a5ab9d1306331f1462ee75f2d6697b7bbf0cce6
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
- Physical multi-domain quality: True
- Online effect learning: True
- Performance win: False
- Bounded churn: True
- Full quality recovered: True
- Backend clean: True

## Mega C / Stage 15
- Verdict: **FAIL**
- Observer-only inference: True
- Truth labels supplied to inference: **NO**
- Truth labels supplied to controller: **NO**
- Resource semantic coverage: 89.17%
- Resource mean confidence: 0.803
- Scene retrieval accuracy: 100%
- Same-scene threshold recall: 100%
- Positive identity margin: 100%
- Mean identity margin: 0.0119
- Cluster recurrence: 100%
- Discovered clusters: 1
