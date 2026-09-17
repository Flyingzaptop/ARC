# ARC -> PS5 Emulator Integration Contract

This is the host contract for the first emulator integration. ARC remains backend-neutral at the policy layer; the emulator supplies D3D12 object/fence bindings and explicit safety metadata.

## 1. One runtime object

Create one `arc::RuntimeIntegration` for the emulated GPU device/runtime and one `arc::dx12::LiveRuntimeBackend` for the D3D12 device.

```cpp
arc::dx12::LiveRuntimeBackend backend(device);
arc::RuntimeIntegrationConfig config{};
config.coordinator.mode = arc::RuntimeMode::ObserveOnly;
arc::RuntimeIntegration arc_runtime(&backend, config);
```

Start in `ObserveOnly`. Do not start a new title directly in mutation mode.

## 2. Resource IDs and lifecycle

Use the same `ResourceId` produced by the ARC observer/resource graph.

For a pageable D3D12 object that the emulator explicitly allows ARC to evict:

1. `backend.bind_resource(resource_id, resource_or_heap)`
2. `arc_runtime.register_controlled_resource(...)` with `ResidencySafety::ControlledSafe`

For an ARC-managed tiled texture:

1. register its normal observer `ResourceId`
2. register `TextureQualityObject` with `TextureQualitySafety::MipSafe`
3. provide the texture mutation callback described below

On destruction, remove both sides before releasing the native object:

```cpp
arc_runtime.unregister_resource(resource_id);
backend.unbind_resource(resource_id);
```

Unknown resources are never auto-promoted to `ControlledSafe`. Render targets, depth/stencil, UAV/compute data, synchronization/state objects and resources without proven semantics stay observe-only until the emulator adapter classifies them.

## 3. Queue and completion-fence binding

ARC requires one monotonic completion fence identity per queue used by controlled resources. Bind both the observer-side fence ID and the native D3D12 fence pointer:

```cpp
arc_runtime.bind_completion_fence(queue_id, completion_fence_id);
backend.bind_queue_fence(queue_id, completion_fence);
```

The emulator may signal other game/application fences on the same queue. ARC ignores those for residency safety even if their numeric values are larger. Only `FenceSignal` events whose `fence` field matches the explicitly bound completion-fence ID are allowed to attach a residency fence requirement to submitted resources.

`QueueSubmit` creates an external in-flight block. A matching `FenceSignal` still does **not** release it. Release happens only after the host has observed actual GPU completion on that same bound fence:

```cpp
arc_runtime.note_queue_completed(queue_id, completion_fence_id, completed_value);
```

A completion update carrying a different fence ID is rejected. Fence values from different queues or different fence objects are never compared.

## 4. Memory budget

Feed `DXGI_QUERY_VIDEO_MEMORY_INFO` samples through either the normal `MemoryBudgetSample` observer event or `RuntimeIntegration::update_budget`.

Recommended first integration:

- sample once during initialization;
- sample on DXGI budget-change notification;
- also sample periodically (for example every 30-60 presents) while controlled mutation is enabled.

`RuntimeCoordinator` blocks mutation when its budget sample is stale. Budget age is measured in coordinator ticks, not resource-use events, so a frame with thousands of resources does not instantly stale the sample. Planning/observation may continue.

## 5. Tick point

Call one ARC tick from a serialized runtime/governor thread, preferably after processing collected events/fence completions and before or around the present boundary:

```cpp
auto result = arc_runtime.tick(frame_epoch);
```

Do not concurrently call `consume`, lifecycle registration/removal and `tick` from unrelated host threads. Producer-side observation may remain multi-threaded; consume the merged ARC event stream on the serialized integration path.

## 6. Rollout modes

Use the modes in this order:

### ObserveOnly

- resource graph, transition learning and budgets are populated;
- no action is resolved/executed by the coordinator;
- safest bring-up mode.

### PlanOnly

- plans are built and resolved against current safety state;
- no backend mutation is executed;
- compare planned actions with emulator knowledge and telemetry.

### Controlled

- only explicitly registered resources/textures may mutate;
- stale budget blocks mutation;
- unresolved/in-flight actions fail closed;
- repeated backend failures open the circuit breaker and force effective `ObserveOnly`;
- bookkeeping failures open the circuit immediately.

The host must explicitly reset the circuit breaker before mutations resume.

## 7. Tiled texture callback

Whole-resource `Evict/MakeResident` is implemented by `arc::dx12::LiveRuntimeBackend`.

Mip quality transitions are host callbacks because the emulator owns reserved-resource tile heaps and mapping topology:

```cpp
backend.set_texture_mutator([&](const arc::TextureQualityAction& action) {
    // Demote: UpdateTileMappings/unmap or release the exact backing represented
    // by action.from_level -> action.to_level.
    // Promote: recreate/remap that backing and make the content valid again.
    // Return Success only after the physical transition has succeeded.
    return arc::RuntimeBackendStatus::Success;
});
```

The callback must be deterministic for the requested transition and must never mutate unknown/non-mip-safe resources.

## 8. Demand residency

Before a controlled evicted resource is required by emulated work, the emulator integration must honor ARC's demand-residency path (or otherwise guarantee residency before queue execution). Transition prefetch is speculative; demand residency remains the correctness path.

Do not submit GPU work that references an ARC-evicted pageable object until `MakeResident` has completed or an appropriate GPU wait has been inserted.

## 9. Safety classification for first PS5 bring-up

Initial allowlist should be deliberately small:

- immutable/streamed texture resources whose contents can be reloaded;
- emulator-owned caches with known reconstruction path;
- explicitly virtualized tiled textures with known mip topology.

Keep these out of mutation initially:

- command/state/synchronization objects;
- render targets and depth/stencil;
- UAV/compute/storage textures unless semantics are explicitly known;
- buffers carrying shader-visible game state;
- aliasing resources without adapter ownership knowledge;
- anything the emulator adapter cannot classify with confidence.

## 10. First emulator acceptance sequence

For each title/workload:

1. ObserveOnly boot and capture resource/event telemetry.
2. PlanOnly for several minutes; inspect proposed actions and resource classes.
3. Controlled mode for a narrow allowlist.
4. Verify D3D12 Debug Layer, no device removal, no content corruption and no graph/ring drops.
5. Expand the allowlist only after the previous class is proven.
6. Enable tiled mip control separately from whole-resource residency so failures are attributable.

## 11. What ARC already owns

The emulator adapter should **not** reimplement these:

- pressure/emergency hysteresis;
- whole-resource candidate scoring;
- reuse/transition prediction;
- eviction fence safety and external in-flight blocks;
- explicit per-queue completion-fence identity;
- global Evict-vs-Demote arbitration;
- headroom restore arbitration;
- stale-plan resolution checks;
- stale-budget mutation block;
- mutation circuit breaker;
- unknown-resource fail-closed behavior.

The emulator adapter should provide facts and native handles; ARC makes the policy decision.
