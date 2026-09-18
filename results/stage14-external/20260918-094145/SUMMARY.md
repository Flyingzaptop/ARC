# ARC Stage 14 External Renderer Acceptance

- ARC source: 6fe0c46646bed666c5fd77063bcea1387257ac46
- External source: microsoft/DirectX-Graphics-Samples @ 213dd4fd4918ea009dd8f35adee1aff1f2ecaba4
- Renderer: D3D12HelloTexture (patched at acceptance time; upstream not vendored)
- Verdict: **FAIL**
- Native target: **1920x1080**
- Temporal / upscaling / frame generation / dynamic resolution: **OFF**
- Timing source: **D3D12 GPU timestamp queries**

## Integration
- Exact pinned upstream: True
- Resource lifecycle observed: True
- Descriptor path observed: True
- Command submission observed: True
- Fence completion observed: True
- Presentation observed: True
- ResourceGraph clean: True
- Observation/control separation clean: True
- GPU timestamp timing: True

## Performance
- Meaningful baseline pressure: True
- Physical quality actuation: True
- Online effect learning: True
- Miss ratio improved >= 10%: True
- p50 improved >= 5%: True
- p99 regression <= 10%: True
- Bounded governor churn: False
- Full quality recovered: True
- Backend clean: True
