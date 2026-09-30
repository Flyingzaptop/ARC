"""Generate the ARC base D3D12 proxy declarations from the installed SDK.

Only the mechanically forwarded declarations live here. Handwritten behavior is
in frontend.cpp and frontend_overrides.inc. Re-run when changing the SDK.
"""
from pathlib import Path
import os
import re

sdk = Path(os.environ.get("ARC_D3D12_HEADER", r"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\d3d12.h"))
source = sdk.read_text(encoding="utf-8", errors="ignore")
source = re.sub(r"#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)(.*?)#else.*?#endif", lambda m: m.group(1), source, flags=re.S)
out = Path(__file__).parent / "generated"
out.mkdir(exist_ok=True)
types = ["Device", "Device5", "CommandQueue", "CommandAllocator", "GraphicsCommandList", "GraphicsCommandList5", "GraphicsCommandList6", "Resource", "Heap", "DescriptorHeap", "Fence", "RootSignature", "PipelineState", "CommandSignature", "QueryHeap"]
custom = {
    "CreateGraphicsPipelineState": "if (!pDesc) return E_INVALIDARG; auto copy = *pDesc; copy.pRootSignature = native_arg(copy.pRootSignature); auto hr = create_wrapped([&](void** p) { return native_->CreateGraphicsPipelineState(&copy, riid, p); }, riid, ppPipelineState); if (SUCCEEDED(hr) && ppPipelineState) describe_graphics_pipeline(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppPipelineState))}, *pDesc); return hr;",
    "CreateComputePipelineState": "if (!pDesc) return E_INVALIDARG; auto copy = *pDesc; copy.pRootSignature = native_arg(copy.pRootSignature); auto hr = create_wrapped([&](void** p) { return native_->CreateComputePipelineState(&copy, riid, p); }, riid, ppPipelineState); if (SUCCEEDED(hr) && ppPipelineState) describe_compute_pipeline(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppPipelineState))}, *pDesc); return hr;",
    "CreateCommandList": "auto hr = create_wrapped([&](void** p) { return native_->CreateCommandList(nodeMask, type, native_arg(pCommandAllocator), native_arg(pInitialState), riid, p); }, riid, ppCommandList); if (SUCCEEDED(hr) && ppCommandList) frontend_runtime().reset_command_list(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppCommandList))}, oid(pCommandAllocator), oid(pInitialState)); return hr;",
    "CreateCommittedResource": "auto hr = create_wrapped([&](void** p) { return native_->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, p); }, riidResource, ppvResource); if (SUCCEEDED(hr) && ppvResource && *ppvResource && pDesc) { frontend_runtime().describe_resource(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppvResource))}, native_->GetResourceAllocationInfo(0, 1, pDesc).SizeInBytes); describe_resource_shape(reinterpret_cast<IUnknown*>(*ppvResource),pDesc,ResourceAllocation::Committed); } return hr;",
    "CreatePlacedResource": "auto hr = create_wrapped([&](void** p) { return native_->CreatePlacedResource(native_arg(pHeap), HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, p); }, riid, ppvResource); if (SUCCEEDED(hr) && ppvResource && *ppvResource && pDesc) { frontend_runtime().describe_resource(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppvResource))}, native_->GetResourceAllocationInfo(0, 1, pDesc).SizeInBytes, oid(pHeap), HeapOffset); describe_resource_shape(reinterpret_cast<IUnknown*>(*ppvResource),pDesc,ResourceAllocation::Placed); } return hr;",
    "CreateReservedResource": "auto hr = create_wrapped([&](void** p) { return native_->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, p); }, riid, ppvResource); if (SUCCEEDED(hr) && ppvResource && *ppvResource) { frontend_runtime().describe_resource(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppvResource))}, 0); describe_resource_shape(reinterpret_cast<IUnknown*>(*ppvResource),pDesc,ResourceAllocation::Reserved); } return hr;",
    "CreateRootSignature": "auto hr = create_wrapped([&](void** p) { return native_->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, p); }, riid, ppvRootSignature); if (SUCCEEDED(hr) && ppvRootSignature && *ppvRootSignature) describe_root_signature(ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppvRootSignature))}, pBlobWithRootSignature, blobLengthInBytes); return hr;",
    "MakeResident": "return resident_objects(native_, NumObjects, ppObjects, true);",
    "Evict": "return resident_objects(native_, NumObjects, ppObjects, false);",
    "SetEventOnMultipleFenceCompletion": "return multi_fence_event(native_, ppFences, pFenceValues, NumFences, Flags, hEvent);",
    "SetResidencyPriority": "return set_residency_priority(native_, NumObjects, ppObjects, pPriorities);",
    "EnqueueMakeResident": "return enqueue_resident(native_, Flags, NumObjects, ppObjects, pFenceToSignal, FenceValueToSignal);",
    "CreatePipelineState": "return create_pipeline_stream(native_, pDesc, riid, ppPipelineState);",
    "CreateCommandList1": "auto hr=create_wrapped([&](void** p){return native_->CreateCommandList1(nodeMask,type,flags,riid,p);},riid,ppCommandList); if(SUCCEEDED(hr)&&ppCommandList&&*ppCommandList){ auto id=ObjectId{object_id(reinterpret_cast<IUnknown*>(*ppCommandList))}; frontend_runtime().reset_command_list(id,{}); frontend_runtime().close_command_list(id); } return hr;",
    "CreateStateObject": "frontend_runtime().unsupported({},\"CreateStateObject DXR object graph\"); if(ppStateObject)*ppStateObject=nullptr; return E_NOINTERFACE;",
    "CreateShaderResourceView": "descriptor_write(DestDescriptor, pResource, ViewKind::Srv); native_->CreateShaderResourceView(native_arg(pResource), pDesc, DestDescriptor);",
    "CreateUnorderedAccessView": "descriptor_write(DestDescriptor, pResource, ViewKind::Uav); native_->CreateUnorderedAccessView(native_arg(pResource), native_arg(pCounterResource), pDesc, DestDescriptor);",
    "CreateRenderTargetView": "descriptor_write(DestDescriptor, pResource, ViewKind::Rtv); native_->CreateRenderTargetView(native_arg(pResource), pDesc, DestDescriptor);",
    "CreateDepthStencilView": "descriptor_write(DestDescriptor, pResource, ViewKind::Dsv); native_->CreateDepthStencilView(native_arg(pResource), pDesc, DestDescriptor);",
    "CreateConstantBufferView": "descriptor_write(DestDescriptor, nullptr, ViewKind::Cbv); native_->CreateConstantBufferView(pDesc, DestDescriptor);",
    "CreateSampler": "descriptor_write(DestDescriptor, nullptr, ViewKind::Sampler); native_->CreateSampler(pDesc, DestDescriptor);",
    "CopyDescriptorsSimple": "descriptor_copy_simple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType); native_->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);",
    "CopyDescriptors": "descriptor_copy_ranges(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType); native_->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);",
    "ExecuteCommandLists": "execute_lists(native_, object, NumCommandLists, ppCommandLists);",
    "Signal": "auto hr = native_->Signal(native_arg(pFence), Value); if (SUCCEEDED(hr)) frontend_runtime().signal(object, oid(pFence), Value); return hr;",
    "Wait": "auto hr = native_->Wait(native_arg(pFence), Value); if (SUCCEEDED(hr)) frontend_runtime().wait(object, oid(pFence), Value); return hr;",
    "Reset": "auto hr = native_->Reset(native_arg(pAllocator), native_arg(pInitialState)); if (SUCCEEDED(hr)) frontend_runtime().reset_command_list(object, oid(pAllocator), oid(pInitialState)); return hr;",
    "Close": "auto hr = native_->Close(); if (SUCCEEDED(hr)) frontend_runtime().close_command_list(object); return hr;",
    "ClearState": "frontend_runtime().set_pipeline(object, oid(pPipelineState)); native_->ClearState(native_arg(pPipelineState));",
    "SetPipelineState": "auto* raw=native_arg(pPipelineState); debug_trace(\"SetPipelineState pointers\",pPipelineState,raw,object.value); frontend_runtime().set_pipeline(object, oid(pPipelineState)); native_->SetPipelineState(raw);",
    "DrawInstanced": "frontend_runtime().record_work(object, WorkKind::Draw); native_->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);",
    "DrawIndexedInstanced": "frontend_runtime().record_work(object, WorkKind::DrawIndexed); native_->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);",
    "Dispatch": "frontend_runtime().record_work(object, WorkKind::Dispatch); native_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);",
    "CopyBufferRegion": "std::array<Access,2> access{{{oid(pSrcBuffer),AccessKind::Read,Certainty::Known,SrcOffset,NumBytes},{oid(pDstBuffer),AccessKind::Write,Certainty::Known,DstOffset,NumBytes}}}; frontend_runtime().record_work(object, WorkKind::Copy, access); native_->CopyBufferRegion(native_arg(pDstBuffer), DstOffset, native_arg(pSrcBuffer), SrcOffset, NumBytes);",
    "CopyResource": "std::array<Access,2> access{{{oid(pSrcResource),AccessKind::Read,Certainty::Known,0,0,\"whole-resource\"},{oid(pDstResource),AccessKind::Write,Certainty::Known,0,0,\"whole-resource\"}}}; frontend_runtime().record_work(object, WorkKind::Copy, access); native_->CopyResource(native_arg(pDstResource), native_arg(pSrcResource));",
    "ResolveSubresource": "std::array<Access,2> access{{{oid(pSrcResource),AccessKind::Read,Certainty::Symbolic,SrcSubresource,0,\"resolve source subresource\"},{oid(pDstResource),AccessKind::Write,Certainty::Symbolic,DstSubresource,0,\"resolve destination subresource\"}}}; frontend_runtime().record_work(object, WorkKind::Resolve, access); native_->ResolveSubresource(native_arg(pDstResource), DstSubresource, native_arg(pSrcResource), SrcSubresource, Format);",
    "ExecuteIndirect": "frontend_runtime().record_work(object, WorkKind::ExecuteIndirect); native_->ExecuteIndirect(native_arg(pCommandSignature), MaxCommandCount, native_arg(pArgumentBuffer), ArgumentBufferOffset, native_arg(pCountBuffer), CountBufferOffset);",
    "ResourceBarrier": "resource_barriers(native_, object, NumBarriers, pBarriers);",
    "ExecuteBundle": "frontend_runtime().execute_bundle(object, {oid(pCommandList)}); native_->ExecuteBundle(native_arg(pCommandList));",
    "SetDescriptorHeaps": "set_descriptor_heaps(native_, object, NumDescriptorHeaps, ppDescriptorHeaps);",
    "SetComputeRootSignature": "frontend_runtime().set_root_signature(object, oid(pRootSignature), true); native_->SetComputeRootSignature(native_arg(pRootSignature));",
    "SetGraphicsRootSignature": "debug_trace(\"SetGraphicsRootSignature begin\", native_, pRootSignature, object.value); frontend_runtime().set_root_signature(object, oid(pRootSignature), false); native_->SetGraphicsRootSignature(native_arg(pRootSignature)); debug_trace(\"SetGraphicsRootSignature end\", native_, pRootSignature, object.value);",
    "SetComputeRootDescriptorTable": "frontend_runtime().set_root_table(object, true, RootParameterIndex, resolve_gpu_descriptor(BaseDescriptor)); native_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);",
    "SetGraphicsRootDescriptorTable": "frontend_runtime().set_root_table(object, false, RootParameterIndex, resolve_gpu_descriptor(BaseDescriptor)); native_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);",
    "SetComputeRoot32BitConstant": "frontend_runtime().set_root_constants(object, true, RootParameterIndex, DestOffsetIn32BitValues, std::span(&SrcData, 1)); native_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);",
    "SetGraphicsRoot32BitConstant": "frontend_runtime().set_root_constants(object, false, RootParameterIndex, DestOffsetIn32BitValues, std::span(&SrcData, 1)); native_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);",
    "SetComputeRoot32BitConstants": "if (pSrcData) frontend_runtime().set_root_constants(object, true, RootParameterIndex, DestOffsetIn32BitValues, {static_cast<const UINT*>(pSrcData), Num32BitValuesToSet}); native_->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);",
    "SetGraphicsRoot32BitConstants": "if (pSrcData) frontend_runtime().set_root_constants(object, false, RootParameterIndex, DestOffsetIn32BitValues, {static_cast<const UINT*>(pSrcData), Num32BitValuesToSet}); native_->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);",
    "SetComputeRootConstantBufferView": "frontend_runtime().set_root_descriptor(object, true, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);",
    "SetGraphicsRootConstantBufferView": "frontend_runtime().set_root_descriptor(object, false, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);",
    "SetComputeRootShaderResourceView": "frontend_runtime().set_root_descriptor(object, true, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);",
    "SetGraphicsRootShaderResourceView": "frontend_runtime().set_root_descriptor(object, false, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);",
    "SetComputeRootUnorderedAccessView": "frontend_runtime().set_root_descriptor(object, true, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);",
    "SetGraphicsRootUnorderedAccessView": "frontend_runtime().set_root_descriptor(object, false, RootParameterIndex, resource_at(BufferLocation), BufferLocation); native_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);",
    "RSSetViewports": "std::vector<Viewport> values; for (UINT i=0;i<NumViewports && pViewports;++i) values.push_back({pViewports[i].TopLeftX,pViewports[i].TopLeftY,pViewports[i].Width,pViewports[i].Height,pViewports[i].MinDepth,pViewports[i].MaxDepth}); frontend_runtime().set_viewports(object,values); native_->RSSetViewports(NumViewports,pViewports);",
    "IASetPrimitiveTopology": "frontend_runtime().set_topology(object,UINT(PrimitiveTopology)); native_->IASetPrimitiveTopology(PrimitiveTopology);",
    "OMSetBlendFactor": "if (BlendFactor) frontend_runtime().set_blend_factor(object,{BlendFactor[0],BlendFactor[1],BlendFactor[2],BlendFactor[3]}); else frontend_runtime().touch_command(object); native_->OMSetBlendFactor(BlendFactor);",
    "OMSetStencilRef": "frontend_runtime().set_stencil_ref(object,StencilRef); native_->OMSetStencilRef(StencilRef);",
    "IASetIndexBuffer": "if (pView) frontend_runtime().set_index_buffer(object, resource_at(pView->BufferLocation), resource_offset(pView->BufferLocation), pView->SizeInBytes); native_->IASetIndexBuffer(pView);",
    "IASetVertexBuffers": "for (UINT i = 0; i < NumViews; ++i) frontend_runtime().set_vertex_buffer(object, StartSlot+i, resource_at(pViews[i].BufferLocation), resource_offset(pViews[i].BufferLocation), pViews[i].SizeInBytes); native_->IASetVertexBuffers(StartSlot, NumViews, pViews);",
    "OMSetRenderTargets": "set_targets(object, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); native_->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);",
    "RSSetScissorRects": "if (NumRects && pRects) frontend_runtime().set_scissor(object, pRects[0].left, pRects[0].top, pRects[0].right - pRects[0].left, pRects[0].bottom - pRects[0].top); if (NumRects>1) frontend_runtime().unsupported(object,\"multiple scissor rectangles\"); native_->RSSetScissorRects(NumRects, pRects);",
    "ClearRenderTargetView": "clear_rtv(native_, object, RenderTargetView, ColorRGBA, NumRects, pRects);",
    "ClearDepthStencilView": "frontend_runtime().record_work(object, WorkKind::Clear); native_->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);",
    "ClearUnorderedAccessViewUint": "Access access{oid(pResource),AccessKind::Write,Certainty::Known,0,0,\"clear UAV\"}; frontend_runtime().record_work(object, WorkKind::Clear, std::span(&access,1)); native_->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, native_arg(pResource), Values, NumRects, pRects);",
    "ClearUnorderedAccessViewFloat": "Access access{oid(pResource),AccessKind::Write,Certainty::Known,0,0,\"clear UAV\"}; frontend_runtime().record_work(object, WorkKind::Clear, std::span(&access,1)); native_->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, native_arg(pResource), Values, NumRects, pRects);",
    "DiscardResource": "frontend_runtime().touch_command(object); native_->DiscardResource(native_arg(pResource), pRegion);",
    "RSSetShadingRate": "if (combiners) { std::array<UINT,2> values{UINT(combiners[0]),UINT(combiners[1])}; frontend_runtime().set_shading_rate(object, UINT(baseShadingRate), values); } else frontend_runtime().set_shading_rate(object, UINT(baseShadingRate)); native_->RSSetShadingRate(baseShadingRate, combiners);",
    "RSSetShadingRateImage": "frontend_runtime().unsupported(object, \"RSSetShadingRateImage\"); native_->RSSetShadingRateImage(native_arg(shadingRateImage));",
    "DispatchMesh": "frontend_runtime().unsupported(object, \"DispatchMesh\"); native_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);",
    "BeginRenderPass": "begin_render_pass(native_, object, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);",
    "EndRenderPass": "frontend_runtime().unsupported(object, \"EndRenderPass\"); native_->EndRenderPass();",
    "AtomicCopyBufferUINT": "atomic_copy(native_, object, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges, false);",
    "AtomicCopyBufferUINT64": "atomic_copy(native_, object, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges, true);",
    "ResolveSubresourceRegion": "std::array<Access,2> access{{{oid(pSrcResource),AccessKind::Read,Certainty::Symbolic,SrcSubresource,0,\"resolve region source\"},{oid(pDstResource),AccessKind::Write,Certainty::Symbolic,DstSubresource,0,\"resolve region destination\"}}}; frontend_runtime().record_work(object, WorkKind::Resolve, access); native_->ResolveSubresourceRegion(native_arg(pDstResource), DstSubresource, DstX, DstY, native_arg(pSrcResource), SrcSubresource, pSrcRect, Format, ResolveMode);",
    "OMSetDepthBounds": "frontend_runtime().unsupported(object,\"OMSetDepthBounds\"); native_->OMSetDepthBounds(Min,Max);",
    "SetSamplePositions": "frontend_runtime().unsupported(object,\"SetSamplePositions\"); native_->SetSamplePositions(NumSamplesPerPixel,NumPixels,pSamplePositions);",
    "SetViewInstanceMask": "frontend_runtime().unsupported(object,\"SetViewInstanceMask\"); native_->SetViewInstanceMask(Mask);",
    "WriteBufferImmediate": "frontend_runtime().unsupported(object,\"WriteBufferImmediate\"); native_->WriteBufferImmediate(Count,pParams,pModes);",
    "SetProtectedResourceSession": "frontend_runtime().unsupported(object,\"SetProtectedResourceSession\"); native_->SetProtectedResourceSession(native_arg(pProtectedResourceSession));",
    "InitializeMetaCommand": "frontend_runtime().unsupported(object,\"InitializeMetaCommand\"); native_->InitializeMetaCommand(native_arg(pMetaCommand),pInitializationParametersData,InitializationParametersDataSizeInBytes);",
    "ExecuteMetaCommand": "frontend_runtime().unsupported(object,\"ExecuteMetaCommand\"); native_->ExecuteMetaCommand(native_arg(pMetaCommand),pExecutionParametersData,ExecutionParametersDataSizeInBytes);",
    "BuildRaytracingAccelerationStructure": "frontend_runtime().unsupported(object,\"BuildRaytracingAccelerationStructure\"); native_->BuildRaytracingAccelerationStructure(pDesc,NumPostbuildInfoDescs,pPostbuildInfoDescs);",
    "EmitRaytracingAccelerationStructurePostbuildInfo": "frontend_runtime().unsupported(object,\"EmitRaytracingAccelerationStructurePostbuildInfo\"); native_->EmitRaytracingAccelerationStructurePostbuildInfo(pDesc,NumSourceAccelerationStructures,pSourceAccelerationStructureData);",
    "CopyRaytracingAccelerationStructure": "frontend_runtime().unsupported(object,\"CopyRaytracingAccelerationStructure\"); native_->CopyRaytracingAccelerationStructure(DestAccelerationStructureData,SourceAccelerationStructureData,Mode);",
    "SetPipelineState1": "frontend_runtime().unsupported(object,\"SetPipelineState1\"); native_->SetPipelineState1(native_arg(pStateObject));",
    "DispatchRays": "frontend_runtime().unsupported(object,\"DispatchRays\"); native_->DispatchRays(pDesc);",
    "BeginQuery": "frontend_runtime().query(object, oid(pQueryHeap)); native_->BeginQuery(native_arg(pQueryHeap), Type, Index);",
    "EndQuery": "frontend_runtime().query(object, oid(pQueryHeap)); native_->EndQuery(native_arg(pQueryHeap), Type, Index);",
    "ResolveQueryData": "frontend_runtime().query(object, oid(pQueryHeap), oid(pDestinationBuffer)); native_->ResolveQueryData(native_arg(pQueryHeap), Type, StartIndex, NumQueries, native_arg(pDestinationBuffer), AlignedDestinationBufferOffset);",
    "SetPredication": "frontend_runtime().unsupported(object, \"SetPredication\"); native_->SetPredication(native_arg(pBuffer), AlignedBufferOffset, Operation);",
    "CopyTextureRegion": "copy_texture_region(native_, object, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);",
}
common_custom = {
    "GetPrivateData": "return get_private_data(guid, pDataSize, pData);",
    "SetPrivateData": "return set_private_bytes(guid, DataSize, pData);",
    "SetPrivateDataInterface": "return set_private_interface(guid, pData);",
}
parents = {}
methods = {}
for match in re.finditer(r"\b(ID3D12\w+)\s*:\s*public\s+(I\w+)\s*\{\s*public:(.*?)\n\s*\};", source, re.S):
    name, parent, body = match.groups()
    parents[name] = parent
    found = []
    for m in re.finditer(r"virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;", body, re.S):
        ret, method, params = m.groups()
        # SAL macros carry nested parentheses; eliminate them before finding names.
        old = None
        while old != params:
            old = params
            params = re.sub(r"\b_[A-Za-z0-9_]+_\([^()]*\([^()]*\)[^()]*\)", "", params)
            params = re.sub(r"\b_[A-Za-z0-9_]+_\([^()]*\)", "", params)
        params = re.sub(r"\b_[A-Za-z0-9_]+_\b", "", params)
        params = re.sub(r"/\*.*?\*/", "", params, flags=re.S)
        params = re.sub(r"\s+", " ", params).strip()
        args = []
        for p in params.split(","):
            p = p.strip()
            if p and p != "void":
                ident = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^]]*\])?$", p)
                if not ident:
                    raise ValueError((name, method, p))
                args.append(ident.group(1))
        found.append((ret.strip(), method, params, args))
    methods[name] = found

for suffix in types:
    iface = "ID3D12" + suffix
    ancestry = []
    current = iface
    while current in methods:
        ancestry.insert(0, current)
        current = parents[current]
    all_methods = {}
    for parent in ancestry:
        for method in methods[parent]:
            all_methods[method[1]] = method
    lines = [f"// Generated from {sdk.name}; changes belong in generate_frontend.py or overrides.", f"class {suffix}Proxy final : public {iface}, public ProxyBase {{", "public:", f"    {suffix}Proxy({iface}* native, ObjectId id) : ProxyBase(native, id), native_(native) {{}}", "    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return query(iid, out); }", "    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }", "    ULONG STDMETHODCALLTYPE Release() override { return release(); }", f"    bool supports(REFIID iid) const noexcept override {{ return iid == __uuidof(IUnknown) || " + " || ".join(f"iid == __uuidof({a})" for a in ancestry) + "; }", "    void* interface_ptr() noexcept override { return static_cast<" + iface + "*>(this); }", "    const char* type_name() const noexcept override { return \"" + suffix + "\"; }", "private:", f"    {iface}* native_;", "public:"]
    for ret, name, params, args in all_methods.values():
        if name in ("QueryInterface", "AddRef", "Release"):
            continue
        callargs = ", ".join(f"native_arg({a})" for a in args)
        if name in common_custom:
            body = common_custom[name]
        elif name in custom and (suffix.startswith("Device") or suffix == "CommandQueue" or suffix.startswith("GraphicsCommandList")):
            body = custom[name]
        elif name == "GetDevice":
            body = "return get_device(riid, ppvDevice);"
        elif "REFIID" in params and "void **" in params and args and args[-1].startswith("pp"):
            prefix = ", ".join(f"native_arg({a})" for a in args[:-1])
            body = f"return create_wrapped([&](void** p) {{ return native_->{name}({prefix}{', ' if prefix else ''}p); }}, {args[-2]}, {args[-1]});"
        else:
            body = f"{'return ' if ret != 'void' else ''}native_->{name}({callargs});"
        lines.append(f"    {ret} STDMETHODCALLTYPE {name}({params}) override {{ debug_trace(\"{suffix}.{name}\", this, native_, object.value); {body} }}")
    lines.append("};")
    (out / f"{suffix.lower()}_proxy.inc").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(suffix, len(all_methods), ancestry)
