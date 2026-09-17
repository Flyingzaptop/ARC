# ARC Mega Stage B — Native D3D12 Host Integration

Mega Stage B covers Stage 13 (Native D3D12 Host Adapter) and Stage 14 (integration into a real D3D12 renderer path).

## Scope

The Stage B adapter is cooperative. It is intended for renderers, engine plugins, open-source D3D12 applications, and other integrations that can call ARC after successful D3D12 operations.

It deliberately does **not**:

- inject a DLL into an arbitrary process;
- patch COM vtables;
- bypass anti-cheat or protected-process mechanisms;
- assume that every observed resource is safe to mutate;
- replace the renderer's descriptors, barriers, command submission, or synchronization.

Observation and control are separate capabilities. ARC may observe a resource without being allowed to mutate it. Residency control is explicit opt-in through `enable_residency_control`; unknown/external resources remain read-only.

## Integration sequence

A native host owns its normal D3D12 objects and constructs:

```cpp
arc::dx12::NativeHostAdapterConfig config{};
config.runtime.coordinator.mode = arc::RuntimeMode::Controlled;
arc::dx12::NativeHostAdapter arcHost(device, adapter3, config);
```

The host then mirrors successful API operations to ARC:

```cpp
const auto queueId = arcHost.observe_queue(queue, D3D12_COMMAND_LIST_TYPE_DIRECT);
const auto commandId = arcHost.observe_command_list(commandList, D3D12_COMMAND_LIST_TYPE_DIRECT);
const auto fenceId = arcHost.bind_completion_fence(queue, completionFence);

const auto resourceId = arcHost.observe_committed_resource(texture);
arcHost.observe_descriptor_heap(srvHeap);
arcHost.observe_srv(srvHeap, descriptorIndex, texture, srvDescription);

arcHost.observe_command_list_reset(commandList);
arcHost.observe_resource_use(commandList, texture, false);
arcHost.observe_transition_barrier(commandList, texture, barrier);
arcHost.observe_command_list_closed(commandList);
arcHost.observe_execute_command_lists(queue, commandLists);

queue->Signal(completionFence, fenceValue);
arcHost.observe_fence_signal(queue, completionFence, fenceValue);
arcHost.poll_completion(queue);

swapchain->Present(syncInterval, flags);
arcHost.observe_present(swapchainId, syncInterval, flags, S_OK);
```

At a frame/control boundary, feed measured frame telemetry and run the unified governor:

```cpp
arc::FrameBudgetSample frame{};
frame.frame_ms = measuredGpuMs;
frame.target_frame_ms = targetGpuMs;
frame.gpu_busy_fraction = gpuBusy;

const auto result = arcHost.frame_tick(frame);
```

A renderer that already owns reliable budget telemetry may call `observe_memory_budget(...)` and use `frame_tick(frame, false)` instead of querying DXGI again.

## Resource classes

### Committed / placed / reserved

Use the matching observation call after successful creation. ARC assigns a stable `ResourceId` for the lifetime of the D3D12 object pointer.

### External resources

Swapchain buffers and other allocations whose ownership/accounting is external to the integrating renderer use:

```cpp
arcHost.observe_external_resource(backBuffer);
```

They are visible in `ResourceGraph`, descriptor usage, barriers, command submissions, and frame analysis, but external observation does not grant mutation permission.

### Controlled resources

A resource becomes eligible for ARC residency mutation only through explicit opt-in:

```cpp
arcHost.enable_residency_control(resource, 0.20);
```

Destroy/unregister the ARC view before or together with the renderer's logical destruction notification:

```cpp
arcHost.observe_resource_destroyed(resource);
```

The adapter removes live-runtime/backend bindings and then emits the lifecycle event into the graph.

## Fence contract

ARC requires one monotonic completion fence for each queue whose controlled resources may be in flight. The host may reuse its normal engine fence. ARC does not signal it independently.

For each queue:

1. call `bind_completion_fence(queue, fence)` once;
2. after the application successfully calls `queue->Signal`, report `observe_fence_signal`;
3. periodically call `poll_completion` / `poll_all_completions`.

Read-only resources are allowed in the same command list. They remain visible to the graph but do not enter the residency controller's in-flight accounting.

## Threading model

The Stage B implementation serializes host-adapter ingress with a mutex before writing to ARC's event ring. This is intentionally conservative: correctness and globally ordered events come before peak ingestion throughput. A future high-throughput adapter can replace this with per-thread rings while preserving the same public contract.

## Stage B reference renderer

`samples/mega_stage_b_host_renderer.cpp` is a real Win32/DXGI/D3D12 reference renderer. It creates a native swapchain, records and submits graphics command lists, uses real RTV/SRV descriptors and resource barriers, reports fence signals/completion, calls `Present`, and measures GPU time with D3D12 timestamp queries.

The renderer exposes three physical quality domains to ARC:

- texture sampling work;
- raster/overdraw layers;
- lighting/shader iterations.

It also registers two controlled residency buffers so the unified memory path is exercised independently of read-only swapchain/scene resources.

The acceptance run is native 1920×1080 with temporal reconstruction, dynamic resolution, DLSS/FSR and frame generation disabled.

## Acceptance gates

`scripts/mega-stage-b-benchmark.ps1` requires all of the following:

- valid Mega Stage B schema and native cooperative integration identity;
- 1920×1080 native rendering with temporal assistance disabled;
- deterministic host-adapter, global-arbiter, and unified-governor regressions;
- resource lifecycle, descriptors, command submission, fences and Present observed through the host adapter;
- clean `ResourceGraph` and zero observation/control contract rejections;
- physical multi-domain quality actuation and online effect learning;
- executed global memory path, restore path and combined arbitration coverage;
- improved target tracking;
- full quality and residency recovery;
- zero quality backend failures.

Successful runs are published to an immutable `results/mega-stage-b-<timestamp>` branch under `results/mega-stage-b/<timestamp>`.

## What Stage B proves — and does not prove

A passing Stage B demonstrates that ARC can sit behind a real D3D12 renderer/engine boundary rather than a benchmark-specific governor API: it can reconstruct resources and command usage, maintain fence-safe controlled lifetimes, observe actual presentation, and actuate/recover quality and residency through one unified runtime.

It does not by itself prove compatibility with arbitrary closed commercial games. That requires a target-specific safe integration mechanism and, later, automatic scene/resource semantics and compatibility policy. The Stage B host contract is the foundation for those integrations.
