param(
    [Parameter(Mandatory=$true)][string]$WickedRoot
)

$ErrorActionPreference = 'Stop'
$LF = [string][char]10
$T = [string][char]9

function Read-Lf([string]$Path) {
    if (-not (Test-Path $Path)) { throw "Missing file: $Path" }
    $x = [IO.File]::ReadAllText($Path)
    return $x.Replace(([string][char]13 + [string][char]10), $LF)
}
function Write-Utf8Lf([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Replace-Once([string]$Text, [string]$Old, [string]$New, [string]$Label) {
    $crlf = [string][char]13 + [string][char]10
    $Old = $Old.Replace($crlf, $LF)
    $New = $New.Replace($crlf, $LF)
    $i = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($i -lt 0) { throw "Patch anchor not found: $Label" }
    if ($Text.IndexOf($Old, $i + $Old.Length, [StringComparison]::Ordinal) -ge 0) {
        throw "Patch anchor not unique: $Label"
    }
    return $Text.Substring(0,$i) + $New + $Text.Substring($i + $Old.Length)
}
function Replace-Nth([string]$Text, [string]$Old, [string]$New, [int]$Occurrence, [string]$Label) {
    $crlf = [string][char]13 + [string][char]10
    $Old = $Old.Replace($crlf, $LF)
    $New = $New.Replace($crlf, $LF)
    $from = 0
    $i = -1
    for ($n = 1; $n -le $Occurrence; ++$n) {
        $i = $Text.IndexOf($Old, $from, [StringComparison]::Ordinal)
        if ($i -lt 0) { throw "Patch anchor #$Occurrence not found: $Label" }
        $from = $i + $Old.Length
    }
    return $Text.Substring(0,$i) + $New + $Text.Substring($i + $Old.Length)
}
function Insert-AfterFunctionAnchor(
    [string]$Text, [string]$FunctionMarker, [string]$Anchor, [string]$Insert, [string]$Label) {
    $f = $Text.IndexOf($FunctionMarker, [StringComparison]::Ordinal)
    if ($f -lt 0) { throw "Function marker not found: $Label" }
    $i = $Text.IndexOf($Anchor, $f, [StringComparison]::Ordinal)
    if ($i -lt 0) { throw "Function anchor not found: $Label" }
    $p = $i + $Anchor.Length
    return $Text.Substring(0,$p) + $Insert + $Text.Substring($p)
}

$dxPath = Join-Path $WickedRoot 'WickedEngine\wiGraphicsDevice_DX12.cpp'
$dx = Read-Lf $dxPath

$dx = Replace-Once $dx '#include "wiGraphicsDevice_DX12.h"' ('#include "wiGraphicsDevice_DX12.h"' + $LF + '#include "ArcWickedHooks.h"') 'DX12 include'

$deviceReadyAnchor = $T+$T+'// Create frame-resident resources:'
$deviceReadyInsert = $T+$T+'ARCWickedDeviceReady(device.Get(), descriptorheap_res.heap_GPU.Get(), descriptorheap_sam.heap_GPU.Get());'+$LF+$LF+$deviceReadyAnchor
$dx = Replace-Once $dx $deviceReadyAnchor $deviceReadyInsert 'device ready'
$copy = '				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);'
$dx = Replace-Nth $dx $copy ($copy + $LF + '				ARCWickedObserveSRV(device->descriptorheap_res.heap_GPU.Get(), (uint32_t)index, res, &srv);') 2 'bindless SRV'
$dx = Replace-Nth $dx $copy ($copy + $LF + '				ARCWickedObserveUAV(device->descriptorheap_res.heap_GPU.Get(), (uint32_t)index, res, &uav);') 3 'bindless UAV'
$dx = Replace-Nth $dx $copy ($copy + $LF + '				ARCWickedObserveSampler(device->descriptorheap_sam.heap_GPU.Get(), (uint32_t)index);') 4 'bindless sampler'

$bufferViews = $T + $T + '// Create resource views if needed'
$dx = Replace-Once $dx $bufferViews ($T + $T + 'ARCWickedResourceCreated(internal_state->resource.Get());' + $LF + $LF + $bufferViews) 'buffer observation'

$textureDefault = $T + $T + 'if (!has_flag(desc->misc_flags, ResourceMiscFlag::NO_DEFAULT_DESCRIPTORS))'
$dx = Replace-Nth $dx $textureDefault ($T + $T + 'ARCWickedResourceCreated(internal_state->resource.Get());' + $LF + $LF + $textureDefault) 2 'texture observation'

# Resource_DX12 uses deferred destruction. Observing wrapper destruction here would
# make ARC mark resources dead before the last queued GPU use. Hook the actual
# deferred release point in wiGraphicsDevice_DX12.h instead.
$dxHeaderPath = Join-Path $WickedRoot 'WickedEngine\\wiGraphicsDevice_DX12.h'
$dxh = Read-Lf $dxHeaderPath
$dxh = Replace-Once $dxh '#include <wrl/client.h> // ComPtr' ('#include <wrl/client.h> // ComPtr' + $LF + $LF + 'extern "C" void ARCWickedResourceDestroyed(ID3D12Resource* resource) noexcept;') 'DX12 deferred destroy hook declaration'
$releaseOld = @'
				while (!destroyer_resources.empty() && destroyer_resources.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_resources.pop_front();
					// comptr auto delete
				}
'@
$releaseNew = @'
				while (!destroyer_resources.empty() && destroyer_resources.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					ARCWickedResourceDestroyed(destroyer_resources.front().first.Get());
					destroyer_resources.pop_front();
					// comptr auto delete
				}
'@
$dxh = Replace-Once $dxh $releaseOld $releaseNew 'deferred resource destruction'
Write-Utf8Lf $dxHeaderPath $dxh

$old = @'
		return cmd;
	}
	void GraphicsDevice_DX12::SubmitCommandLists()
'@
$new = @'
		ARCWickedCommandBegin(commandlist.GetCommandList(), queues[queue].desc.Type);
		return cmd;
	}
	void GraphicsDevice_DX12::SubmitCommandLists()
'@
$dx = Replace-Once $dx $old $new 'command begin'

$old = @'
		queue->ExecuteCommandLists(
			(UINT)submit_cmds.size(),
			submit_cmds.data()
		);

		submit_cmds.clear();
'@
$new = @'
		queue->ExecuteCommandLists(
			(UINT)submit_cmds.size(),
			submit_cmds.data()
		);
		ARCWickedSubmit(queue.Get(), submit_cmds.data(), submit_cmds.size(), desc.Type);

		submit_cmds.clear();
'@
$dx = Replace-Once $dx $old $new 'queue submit'

$present = $T+$T+$T+$T+$T+'HRESULT hr = dx12_check(swapchain_internal->swapChain->Present(swapchain->desc.vsync, presentFlags));'
$dx = Replace-Once $dx $present ($present + $LF + $T+$T+$T+$T+$T+'ARCWickedPresent((uint64_t)swapchain_internal->swapChain.Get(), swapchain->desc.vsync, presentFlags, hr);') 'present'

$anchor = $T+$T+'CommandList_DX12& commandlist = GetCommandList(cmd);'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::BindResource(' $anchor ($LF+$T+$T+'if (resource != nullptr && resource->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(resource)->resource.Get(), false);') 'BindResource'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::BindUAV(' $anchor ($LF+$T+$T+'if (resource != nullptr && resource->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(resource)->resource.Get(), true);') 'BindUAV'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::BindConstantBuffer(' $anchor ($LF+$T+$T+'if (buffer != nullptr && buffer->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(buffer)->resource.Get(), false);') 'BindConstantBuffer'

$vertex = $LF+$T+$T+'for (uint32_t arc_i = 0; arc_i < count; ++arc_i)'+$LF+
          $T+$T+'{'+$LF+
          $T+$T+$T+'if (vertexBuffers[arc_i] != nullptr && vertexBuffers[arc_i]->IsValid())'+$LF+
          $T+$T+$T+$T+'ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(vertexBuffers[arc_i])->resource.Get(), false);'+$LF+
          $T+$T+'}'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::BindVertexBuffers(' $anchor $vertex 'BindVertexBuffers'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::BindIndexBuffer(' $anchor ($LF+$T+$T+'if (indexBuffer != nullptr && indexBuffer->IsValid()) ARCWickedResourceUse(commandlist.GetCommandList(), to_internal(indexBuffer)->resource.Get(), false);') 'BindIndexBuffer'

# Render-pass attachments are not shader bindings, so they need explicit
# behavioral observation. Without this, depth/render targets disappear from
# scene windows and very different scenes collapse to the same signature.
$renderPassMarker = 'void GraphicsDevice_DX12::RenderPassBegin(const RenderPassImage* images, uint32_t image_count, CommandList cmd, RenderPassFlags flags)'
$renderPassAnchor = $T+$T+$T+'auto internal_state = to_internal(texture);'
$renderPassUse = $LF+
    $T+$T+$T+'const bool arc_write = image.type != RenderPassImage::Type::SHADING_RATE_SOURCE;'+$LF+
    $T+$T+$T+'ARCWickedResourceUse(commandlist.GetCommandList(), internal_state->resource.Get(), arc_write);'
$dx = Insert-AfterFunctionAnchor $dx $renderPassMarker $renderPassAnchor $renderPassUse 'RenderPass attachments'

$counterHooks = @(
    @('void GraphicsDevice_DX12::Draw(',0),
    @('void GraphicsDevice_DX12::DrawIndexed(',1),
    @('void GraphicsDevice_DX12::DrawInstanced(',0),
    @('void GraphicsDevice_DX12::DrawIndexedInstanced(',1),
    @('void GraphicsDevice_DX12::DrawInstancedIndirect(',3),
    @('void GraphicsDevice_DX12::DrawIndexedInstancedIndirect(',3),
    @('void GraphicsDevice_DX12::Dispatch(',2),
    @('void GraphicsDevice_DX12::DispatchIndirect(',3),
    @('void GraphicsDevice_DX12::DispatchMesh(',0),
    @('void GraphicsDevice_DX12::DispatchMeshIndirect(',3)
)
foreach ($entry in $counterHooks) {
    $dx = Insert-AfterFunctionAnchor $dx $entry[0] $anchor ($LF+$T+$T+"ARCWickedCountCommand(commandlist.GetCommandList(), $($entry[1]));") $entry[0]
}

$copyAnchor = $T+$T+'auto internal_state_dst = to_internal(pDst);'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::CopyResource(' $copyAnchor ($LF+$T+$T+'ARCWickedCopy(commandlist.GetCommandList(), internal_state_src->resource.Get(), internal_state_dst->resource.Get(), 0, 0);') 'CopyResource'
$copyAnchor = $T+$T+'auto dst_internal = to_internal(pDst);'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::CopyBuffer(' $copyAnchor ($LF+$T+$T+'ARCWickedCopy(commandlist.GetCommandList(), src_internal->resource.Get(), dst_internal->resource.Get(), size, 1);') 'CopyBuffer'
$copyAnchor = $T+$T+'auto dst_internal = to_internal(dst);'
$dx = Insert-AfterFunctionAnchor $dx 'void GraphicsDevice_DX12::CopyTexture(' $copyAnchor ($LF+$T+$T+'ARCWickedCopy(commandlist.GetCommandList(), src_internal->resource.Get(), dst_internal->resource.Get(), 0, 2);') 'CopyTexture'

$push = $T+$T+$T+'barrierdescs.push_back(barrierdesc);'
$hook = @'
			if (barrierdesc.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barrierdesc.Transition.pResource != nullptr)
			{
				ARCWickedTransition(
					commandlist.GetCommandList(),
					barrierdesc.Transition.pResource,
					barrierdesc.Transition.StateBefore,
					barrierdesc.Transition.StateAfter,
					barrierdesc.Transition.Subresource
				);
			}
			barrierdescs.push_back(barrierdesc);
'@
$dx = Replace-Once $dx $push $hook.TrimEnd() 'barrier telemetry'

Write-Utf8Lf $dxPath $dx
Write-Host 'Wicked DX12 backend patched for ARC.'
