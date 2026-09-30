struct HeapRange { ObjectId id; std::uintptr_t cpu{}, gpu{}; UINT count{}, stride{}; D3D12_DESCRIPTOR_HEAP_TYPE type{}; };
std::vector<HeapRange> descriptor_heaps;
struct BufferRange { ObjectId id; D3D12_GPU_VIRTUAL_ADDRESS address; UINT64 size; };
std::vector<BufferRange> buffers;

ObjectId oid(IUnknown* value) noexcept { return ObjectId{object_id(value)}; }
ResourceShape resource_shape(const D3D12_RESOURCE_DESC& desc, ResourceAllocation allocation) {
    ResourceShape shape{};
    switch (desc.Dimension) {
    case D3D12_RESOURCE_DIMENSION_BUFFER: shape.dimension = ResourceDimension::Buffer; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: shape.dimension = ResourceDimension::Texture1D; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: shape.dimension = ResourceDimension::Texture2D; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D: shape.dimension = ResourceDimension::Texture3D; break;
    default: break;
    }
    shape.allocation = allocation;
    shape.width = desc.Width;
    shape.height = desc.Height;
    shape.depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
    shape.array_layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    shape.mips = desc.MipLevels;
    shape.format = UINT(desc.Format);
    shape.samples = desc.SampleDesc.Count;
    shape.flags = UINT(desc.Flags);
    shape.format_namespace = 1;
    shape.flags_namespace = 1;
    return shape;
}
void describe_resource_shape(IUnknown* wrapper, const D3D12_RESOURCE_DESC* desc,
                             ResourceAllocation allocation) {
    if (wrapper && desc) frontend_runtime().describe_resource_shape(oid(wrapper), resource_shape(*desc, allocation));
}
ObjectId resource_at(D3D12_GPU_VIRTUAL_ADDRESS address) noexcept {
    std::lock_guard lock(registry_mutex);
    ObjectId found{};
    for (auto& buffer : buffers)
        if (address >= buffer.address && address - buffer.address < buffer.size) {
            if (found.value && found != buffer.id) return {};
            found = buffer.id;
        }
    return found;
}
UINT64 resource_offset(D3D12_GPU_VIRTUAL_ADDRESS address) noexcept {
    std::lock_guard lock(registry_mutex);
    UINT64 offset = 0;
    ObjectId found{};
    for (auto& buffer : buffers)
        if (address >= buffer.address && address - buffer.address < buffer.size) {
            if (found.value && found != buffer.id) return 0;
            found = buffer.id;
            offset = address - buffer.address;
        }
    return offset;
}
void register_resource(ObjectId id, ID3D12Resource* resource) {
    const auto desc = resource->GetDesc();
    frontend_runtime().describe_resource_shape(id, resource_shape(desc, ResourceAllocation::External));
    const auto bytes = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? desc.Width : 0;
    frontend_runtime().describe_resource(id, bytes);
    if (!bytes) return;
    const auto address = resource->GetGPUVirtualAddress();
    if (!address) return;
    std::lock_guard lock(registry_mutex);
    buffers.push_back({id, address, bytes});
}
void register_descriptor_heap(ObjectId id, ID3D12DescriptorHeap* heap) {
    debug_trace("register_descriptor_heap", heap, nullptr, id.value);
    const auto desc = heap->GetDesc();
    frontend_runtime().describe_descriptor_heap(id, desc.NumDescriptors);
    auto* device_child = static_cast<ID3D12DeviceChild*>(heap);
    ID3D12Device* device = nullptr;
    if (FAILED(device_child->GetDevice(IID_PPV_ARGS(&device)))) return;
    const auto stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    device->Release();
    if (!stride) return;
    std::lock_guard lock(registry_mutex);
    const auto gpu = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        ? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
    descriptor_heaps.push_back({id, heap->GetCPUDescriptorHandleForHeapStart().ptr,
        gpu, desc.NumDescriptors, stride, desc.Type});
    debug_trace("register_descriptor_heap end", heap, nullptr, id.value);
}
DescriptorRef resolve_cpu(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    std::lock_guard lock(registry_mutex);
    DescriptorRef found{};
    for (auto& heap : descriptor_heaps) {
        if (handle.ptr >= heap.cpu && heap.stride && (handle.ptr - heap.cpu) / heap.stride < heap.count &&
            (handle.ptr - heap.cpu) % heap.stride == 0)
            { auto index = UINT((handle.ptr - heap.cpu) / heap.stride);
              auto known = frontend_runtime().descriptor(heap.id, index);
              if (found.heap.value && found.heap != heap.id) return {};
              found = {heap.id, index, known.generation}; }
    }
    return found;
}
DescriptorRef resolve_gpu_descriptor(D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
    std::lock_guard lock(registry_mutex);
    DescriptorRef found{};
    for (auto& heap : descriptor_heaps) {
        if (heap.gpu && handle.ptr >= heap.gpu && heap.stride && (handle.ptr - heap.gpu) / heap.stride < heap.count &&
            (handle.ptr - heap.gpu) % heap.stride == 0)
            { auto index = UINT((handle.ptr - heap.gpu) / heap.stride);
              auto known = frontend_runtime().descriptor(heap.id, index);
              if (found.heap.value && found.heap != heap.id) return {};
              found = {heap.id, index, known.generation}; }
    }
    return found;
}
void descriptor_write(D3D12_CPU_DESCRIPTOR_HANDLE target, IUnknown* resource, ViewKind kind) {
    auto ref = resolve_cpu(target);
    if (ref.heap.value) frontend_runtime().write_descriptor(ref.heap, ref.index, oid(resource), kind);
}
void descriptor_copy_simple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    for (UINT i = 0; i < count; ++i) {
        auto d = resolve_cpu(dst), s = resolve_cpu(src);
        if (d.heap.value) frontend_runtime().copy_descriptor(d.heap, d.index, s);
        std::lock_guard lock(registry_mutex);
        for (auto& h : descriptor_heaps) if (h.type == type && h.id == d.heap) { dst.ptr += h.stride; break; }
        for (auto& h : descriptor_heaps) if (h.type == type && h.id == s.heap) { src.ptr += h.stride; break; }
    }
}
void descriptor_copy_ranges(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dsz,
                            UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < nd && si < ns) {
        auto d = dst[di], s = src[si];
        auto dr = resolve_cpu(d), sr = resolve_cpu(s);
        UINT stride = 0;
        { std::lock_guard lock(registry_mutex); for (auto& h : descriptor_heaps) if (h.id == dr.heap && h.type == type) stride = h.stride; }
        if (!stride) break;
        d.ptr += doff * stride; s.ptr += soff * stride;
        descriptor_copy_simple(1, d, s, type);
        if (++doff >= (dsz ? dsz[di] : 1)) { ++di; doff = 0; }
        if (++soff >= (ssz ? ssz[si] : 1)) { ++si; soff = 0; }
    }
}
HRESULT resident_objects(ID3D12Device* native, UINT count, ID3D12Pageable* const* objects, bool make) {
    std::vector<ID3D12Pageable*> raw;
    for (UINT i = 0; i < count; ++i) raw.push_back(native_arg(objects[i]));
    return make ? native->MakeResident(count, raw.data()) : native->Evict(count, raw.data());
}
HRESULT multi_fence_event(ID3D12Device1* native, ID3D12Fence* const* fences,
    const UINT64* values, UINT count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) {
    std::vector<ID3D12Fence*> raw;
    for (UINT i = 0; i < count; ++i) raw.push_back(native_arg(fences[i]));
    return native->SetEventOnMultipleFenceCompletion(raw.data(), values, count, flags, event);
}
HRESULT set_residency_priority(ID3D12Device1* native, UINT count,
    ID3D12Pageable* const* objects, const D3D12_RESIDENCY_PRIORITY* priority) {
    std::vector<ID3D12Pageable*> raw;
    for (UINT i = 0; i < count; ++i) raw.push_back(native_arg(objects[i]));
    return native->SetResidencyPriority(count, raw.data(), priority);
}
HRESULT enqueue_resident(ID3D12Device3* native, D3D12_RESIDENCY_FLAGS flags, UINT count,
    ID3D12Pageable* const* objects, ID3D12Fence* fence, UINT64 value) {
    std::vector<ID3D12Pageable*> raw;
    for (UINT i = 0; i < count; ++i) raw.push_back(native_arg(objects[i]));
    auto hr = native->EnqueueMakeResident(flags, count, raw.data(), native_arg(fence), value);
    if (SUCCEEDED(hr)) frontend_runtime().unsupported({}, "EnqueueMakeResident fence edge");
    return hr;
}
struct PipelineField { SIZE_T size{}, alignment{}; };
template<class T> constexpr PipelineField pipeline_field() { return {sizeof(T), alignof(T)}; }
PipelineField pipeline_data_layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return pipeline_field<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        return pipeline_field<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return pipeline_field<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return pipeline_field<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return pipeline_field<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return pipeline_field<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return pipeline_field<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return pipeline_field<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return pipeline_field<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return pipeline_field<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return pipeline_field<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return pipeline_field<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return pipeline_field<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return pipeline_field<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return pipeline_field<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return pipeline_field<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return pipeline_field<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return pipeline_field<D3D12_VIEW_INSTANCING_DESC>();
    default: return {};
    }
}
HRESULT create_pipeline_stream(ID3D12Device2* native, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    REFIID iid, void** out) {
    debug_trace("CreatePipelineState stream", native, desc, desc ? desc->SizeInBytes : 0);
    if (!desc) return E_INVALIDARG;
    if (out) *out = nullptr;
    if (desc->SizeInBytes > 1u << 20 || !desc->pPipelineStateSubobjectStream) return E_INVALIDARG;
    std::vector<std::byte> stream(desc->SizeInBytes);
    memcpy(stream.data(), desc->pPipelineStateSubobjectStream, stream.size());
    struct StageBytecode { D3D12_SHADER_BYTECODE code; const char* stage; };
    std::vector<StageBytecode> stages;
    for (SIZE_T offset = 0; offset < stream.size();) {
        if (stream.size() - offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) return E_INVALIDARG;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        memcpy(&type, stream.data() + offset, sizeof(type));
        auto field = pipeline_data_layout(type);
        if (!field.size) { frontend_runtime().unsupported({}, "unknown pipeline stream subobject"); return E_NOTIMPL; }
        const auto data_offset = (offset + sizeof(type) + field.alignment - 1) & ~(field.alignment - 1);
        const auto next = (data_offset + field.size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        if (next > stream.size()) return E_INVALIDARG;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            ID3D12RootSignature* root = nullptr;
            memcpy(&root, stream.data() + data_offset, sizeof(root));
            root = native_arg(root);
            memcpy(stream.data() + data_offset, &root, sizeof(root));
        }
        const char* stage = nullptr;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: stage = "vs"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: stage = "ps"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: stage = "ds"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: stage = "hs"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: stage = "gs"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: stage = "cs"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: stage = "as"; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: stage = "ms"; break;
        default: break;
        }
        if (stage) {
            D3D12_SHADER_BYTECODE code{};
            memcpy(&code, stream.data() + data_offset, sizeof(code));
            stages.push_back({code, stage});
        }
        offset = next;
    }
    D3D12_PIPELINE_STATE_STREAM_DESC copy{stream.size(), stream.data()};
    if (!out) return native->CreatePipelineState(&copy, iid, nullptr);
    IUnknown* raw = nullptr;
    debug_trace("CreatePipelineState native begin", native, stream.data(), stream.size());
    auto hr = native->CreatePipelineState(&copy, iid, reinterpret_cast<void**>(&raw));
    debug_trace("CreatePipelineState native end", raw, nullptr, static_cast<unsigned>(hr));
    if (FAILED(hr)) return hr;
    auto wrapped = wrap_object(raw, iid, out);
    if (SUCCEEDED(wrapped)) {
        std::vector<ObjectId> shader_ids;
        for (const auto& stage : stages)
            if (auto id = register_shader_stage(stage.code, stage.stage)) shader_ids.push_back(id);
        frontend_runtime().set_pipeline_shaders(oid(reinterpret_cast<IUnknown*>(*out)), shader_ids);
    }
    return FAILED(wrapped) ? wrapped : hr;
}
void execute_lists(ID3D12CommandQueue* native, ObjectId queue, UINT count, ID3D12CommandList* const* lists) {
    std::vector<ID3D12CommandList*> raw; std::vector<ObjectId> ids;
    for (UINT i = 0; i < count; ++i) { raw.push_back(native_arg(lists[i])); ids.push_back(oid(lists[i])); }
    frontend_runtime().submit(queue, ids);
    native->ExecuteCommandLists(count, raw.data());
}
void set_descriptor_heaps(ID3D12GraphicsCommandList* native, ObjectId list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    debug_trace("SetDescriptorHeaps begin", native, heaps, list.value);
    std::vector<ID3D12DescriptorHeap*> raw; std::vector<ObjectId> ids;
    for (UINT i = 0; i < count; ++i) { raw.push_back(native_arg(heaps[i])); ids.push_back(oid(heaps[i])); }
    frontend_runtime().set_descriptor_heaps(list, ids);
    native->SetDescriptorHeaps(count, raw.data());
    debug_trace("SetDescriptorHeaps end", native, nullptr, list.value);
}
void set_targets(ObjectId list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    std::vector<DescriptorRef> rt;
    auto first = count && handles ? resolve_cpu(handles[0]) : DescriptorRef{};
    for (UINT i = 0; i < count; ++i) {
        auto h = handles[contiguous ? 0 : i];
        if (contiguous && i) { std::lock_guard lock(registry_mutex); for (auto& x : descriptor_heaps) if (first.heap == x.id) { h.ptr += i * x.stride; break; } }
        rt.push_back(resolve_cpu(h));
    }
    frontend_runtime().set_targets(list, rt, depth ? resolve_cpu(*depth) : DescriptorRef{});
}
void resource_barriers(ID3D12GraphicsCommandList* native, ObjectId list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> copy(barriers, barriers + count);
    for (auto& b : copy) {
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            frontend_runtime().barrier(list, oid(b.Transition.pResource), b.Transition.StateBefore, b.Transition.StateAfter);
            b.Transition.pResource = native_arg(b.Transition.pResource);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            frontend_runtime().alias_barrier(list, oid(b.Aliasing.pResourceBefore), oid(b.Aliasing.pResourceAfter));
            b.Aliasing.pResourceBefore = native_arg(b.Aliasing.pResourceBefore);
            b.Aliasing.pResourceAfter = native_arg(b.Aliasing.pResourceAfter);
        } else { frontend_runtime().record_work(list, WorkKind::Barrier); b.UAV.pResource = native_arg(b.UAV.pResource); }
    }
    native->ResourceBarrier(count, copy.data());
}
void copy_texture_region(ID3D12GraphicsCommandList* native, ObjectId list,
    const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    std::array<Access, 2> access{{
        {oid(src->pResource), AccessKind::Read, Certainty::Symbolic, 0, 0, "texture copy source subresource"},
        {oid(dst->pResource), AccessKind::Write, Certainty::Symbolic, 0, 0, "texture copy destination subresource"}}};
    frontend_runtime().record_work(list, WorkKind::Copy, access);
    auto d = *dst, s = *src;
    d.pResource = native_arg(d.pResource); s.pResource = native_arg(s.pResource);
    native->CopyTextureRegion(&d, x, y, z, &s, box);
}
void atomic_copy(ID3D12GraphicsCommandList1* native, ObjectId list,
    ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src, UINT64 src_offset,
    UINT dependencies, ID3D12Resource* const* dependent,
    const D3D12_SUBRESOURCE_RANGE_UINT64* ranges, bool wide) {
    std::vector<ID3D12Resource*> raw;
    for (UINT i = 0; i < dependencies; ++i) raw.push_back(native_arg(dependent[i]));
    std::array<Access,2> access{{
        {oid(src),AccessKind::Read,Certainty::Known,src_offset,wide ? 8u : 4u},
        {oid(dst),AccessKind::Write,Certainty::Known,dst_offset,wide ? 8u : 4u}}};
    frontend_runtime().record_work(list, WorkKind::Copy, access);
    if (wide) native->AtomicCopyBufferUINT64(native_arg(dst),dst_offset,native_arg(src),src_offset,
                                             dependencies,raw.data(),ranges);
    else native->AtomicCopyBufferUINT(native_arg(dst),dst_offset,native_arg(src),src_offset,
                                      dependencies,raw.data(),ranges);
}
void begin_render_pass(ID3D12GraphicsCommandList4* native, ObjectId list, UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> copy(targets, targets + count);
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
    for (auto& rt : copy) {
        handles.push_back(rt.cpuDescriptor);
        if (rt.EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
            rt.EndingAccess.Resolve.pSrcResource = native_arg(rt.EndingAccess.Resolve.pSrcResource);
            rt.EndingAccess.Resolve.pDstResource = native_arg(rt.EndingAccess.Resolve.pDstResource);
        }
    }
    auto depth_copy = depth ? *depth : D3D12_RENDER_PASS_DEPTH_STENCIL_DESC{};
    if (depth) {
        for (auto* ending : {&depth_copy.DepthEndingAccess, &depth_copy.StencilEndingAccess})
            if (ending->Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
                ending->Resolve.pSrcResource = native_arg(ending->Resolve.pSrcResource);
                ending->Resolve.pDstResource = native_arg(ending->Resolve.pDstResource);
            }
    }
    set_targets(list, count, handles.data(), FALSE, depth ? &depth->cpuDescriptor : nullptr);
    frontend_runtime().unsupported(list, "BeginRenderPass semantic boundary");
    native->BeginRenderPass(count, copy.data(), depth ? &depth_copy : nullptr, flags);
}
void clear_rtv(ID3D12GraphicsCommandList* native, ObjectId list, D3D12_CPU_DESCRIPTOR_HANDLE handle,
               const FLOAT color[4], UINT count, const D3D12_RECT* rects) {
    std::vector<std::uint32_t> sig;
    for (int i = 0; i < 4; ++i) sig.push_back(std::bit_cast<std::uint32_t>(color[i]));
    sig.push_back(count);
    for (UINT i = 0; i < count; ++i) { sig.push_back(rects[i].left); sig.push_back(rects[i].top); sig.push_back(rects[i].right); sig.push_back(rects[i].bottom); }
    const bool redundant = frontend_runtime().record_clear(list, resolve_cpu(handle), sig);
    auto callback = clear_admit.load();
    if (!callback || !callback(list.value, redundant)) native->ClearRenderTargetView(handle, color, count, rects);
}
