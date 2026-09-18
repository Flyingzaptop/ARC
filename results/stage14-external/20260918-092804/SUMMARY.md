# ARC Stage 14 External Renderer Acceptance

- ARC source: e094237a365d4cd88a7f720107f3cce8bbd694f3
- External source: microsoft/DirectX-Graphics-Samples @ 213dd4fd4918ea009dd8f35adee1aff1f2ecaba4
- Renderer: D3D12HelloTexture (patched at acceptance time; upstream not vendored)
- Verdict: **FAIL**
- Native target: **1920x1080**
- Temporal / upscaling / frame generation / dynamic resolution: **OFF**

## Integration
- Exact pinned upstream: True
- Resource lifecycle observed: True
- Descriptor path observed: True
- Command submission observed: True
- Fence completion observed: True
- Presentation observed: True
- ResourceGraph clean: True
- Observation/control separation clean: True

## Performance
- Meaningful baseline pressure: True
- Physical quality actuation: True
- Online effect learning: True
- Miss ratio improved >= 10%: False
- p50 improved >= 5%: False
- p99 regression <= 10%: True
- Bounded governor churn: False
- Full quality recovered: True
- Backend clean: True
