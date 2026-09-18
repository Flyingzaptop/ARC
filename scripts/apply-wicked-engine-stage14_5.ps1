param(
    [Parameter(Mandatory=$true)][string]$ArcRepoRoot,
    [Parameter(Mandatory=$true)][string]$WickedRoot
)

$ErrorActionPreference = 'Stop'
$PinnedWickedSha = '0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7'

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8)
}
function Replace-Once([string]$Path, [string]$Old, [string]$New, [string]$Label) {
    $text = [System.IO.File]::ReadAllText($Path)
    $first = $text.IndexOf($Old, [System.StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Stage14.5 patch anchor missing: $Label ($Path)" }
    $second = $text.IndexOf($Old, $first + $Old.Length, [System.StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Stage14.5 patch anchor is ambiguous: $Label ($Path)" }
    $text = $text.Substring(0, $first) + $New + $text.Substring($first + $Old.Length)
    Write-Utf8NoBom $Path $text
}
function Replace-AllChecked([string]$Path, [string]$Old, [string]$New, [int]$Expected, [string]$Label) {
    $text = [System.IO.File]::ReadAllText($Path)
    $count = 0
    $cursor = 0
    while (($idx = $text.IndexOf($Old, $cursor, [System.StringComparison]::Ordinal)) -ge 0) {
        $count++
        $cursor = $idx + $Old.Length
    }
    if ($count -ne $Expected) { throw "Stage14.5 patch count mismatch for $Label. Expected $Expected, found $count." }
    $text = $text.Replace($Old, $New)
    Write-Utf8NoBom $Path $text
}

$ArcRepoRoot = (Resolve-Path $ArcRepoRoot).Path
$WickedRoot = (Resolve-Path $WickedRoot).Path
$head = (& git -C $WickedRoot rev-parse HEAD).Trim()
if ($head -ne $PinnedWickedSha) {
    throw "Wicked Engine must be pinned to $PinnedWickedSha, got $head"
}
$status = (& git -C $WickedRoot status --porcelain)
if ($status) { throw 'Wicked Engine checkout must be clean before applying Stage 14.5 overlay.' }

$srcBridge = Join-Path $ArcRepoRoot 'integrations\wicked-engine\ArcWickedBridge.cpp'
$srcBridgeH = Join-Path $ArcRepoRoot 'integrations\wicked-engine\ArcWickedBridge.h'
$srcHooks = Join-Path $ArcRepoRoot 'integrations\wicked-engine\ArcWickedHooks.h'
foreach ($p in @($srcBridge,$srcBridgeH,$srcHooks)) {
    if (-not (Test-Path $p)) { throw "Missing ARC integration source: $p" }
}

$testsDir = Join-Path $WickedRoot 'Samples\Tests'
$engineDir = Join-Path $WickedRoot 'WickedEngine'
Copy-Item $srcBridge (Join-Path $testsDir 'ArcWickedBridge.cpp') -Force
Copy-Item $srcBridgeH (Join-Path $testsDir 'ArcWickedBridge.h') -Force
Copy-Item $srcHooks (Join-Path $engineDir 'ArcWickedHooks.h') -Force

$profH = Join-Path $engineDir 'wiProfiler.h'
Replace-Once $profH @'
	bool IsEnabled();

	void SetBackgroundColor(wi::Color color);
'@ @'
	bool IsEnabled();

	// ARC Stage 14.5: read-only access to the timestamp-backed GPU frame range.
	float GetLastGPUFrameTimeMS();

	void SetBackgroundColor(wi::Color color);
'@ 'profiler header accessor'

$profCpp = Join-Path $engineDir 'wiProfiler.cpp'
Replace-Once $profCpp @'
	bool IsEnabled()
	{
		return ENABLED;
	}

	void SetBackgroundColor(wi::Color color)
'@ @'
	bool IsEnabled()
	{
		return ENABLED;
	}

	float GetLastGPUFrameTimeMS()
	{
		std::scoped_lock guard(lock);
		const auto it = ranges.find(gpu_frame);
		return it == ranges.end() ? 0.0f : it->second.time;
	}

	void SetBackgroundColor(wi::Color color)
'@ 'profiler implementation accessor'

$testsCpp = Join-Path $testsDir 'Tests.cpp'
Replace-Once $testsCpp @'
#include "stdafx.h"

#define CONTENT_DIR
'@ @'
#include "stdafx.h"
#include "ArcWickedBridge.h"

#define CONTENT_DIR
'@ 'Tests bridge include'
Replace-Once $testsCpp @'
void TestsRenderer::Update(float dt)
{
	int selected = testSelector.GetSelected();
'@ @'
void TestsRenderer::Update(float dt)
{
	// Stage 14.5 truth test is native-resolution and temporal-free.
	resolutionScale = 1.0f;
	arc_wicked::HarnessUpdate(testSelector, GetPhysicalWidth(), GetPhysicalHeight());

	int selected = testSelector.GetSelected();
'@ 'Tests harness update'

$mainCpp = Join-Path $testsDir 'main_Windows.cpp'
Replace-Once $mainCpp @'
   if (!hWnd)
   {
      return FALSE;
   }

   tests.SetWindow(hWnd);
'@ @'
   if (!hWnd)
   {
      return FALSE;
   }

   // ARC Stage 14.5 acceptance is explicitly native 1920x1080.
   RECT arcClient = { 0, 0, 1920, 1080 };
   const DWORD arcStyle = static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE));
   const DWORD arcExStyle = static_cast<DWORD>(GetWindowLongW(hWnd, GWL_EXSTYLE));
   if (!AdjustWindowRectExForDpi(&arcClient, arcStyle, TRUE, arcExStyle, GetDpiForWindow(hWnd)))
   {
      return FALSE;
   }
   SetWindowPos(
      hWnd, nullptr, 0, 0,
      arcClient.right - arcClient.left,
      arcClient.bottom - arcClient.top,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

   tests.SetWindow(hWnd);
'@ 'native 1080p client size'

$dx12 = Join-Path $engineDir 'wiGraphicsDevice_DX12.cpp'
Replace-Once $dx12 @'
#include "wiGraphicsDevice_DX12.h"

#ifdef WICKEDENGINE_BUILD_DX12
'@ @'
#include "wiGraphicsDevice_DX12.h"
#include "ArcWickedHooks.h"

#ifdef WICKEDENGINE_BUILD_DX12
'@ 'DX12 hooks include'

Replace-Once $dx12 @'
		~Resource_DX12()
		{
			std::scoped_lock lck(allocationhandler->destroylocker);
			uint64_t framecount = allocationhandler->framecount;
			if (allocation) allocationhandler->destroyer_allocations.push_back(std::make_pair(allocation, framecount));
			if (resource) allocationhandler->destroyer_resources.push_back(std::make_pair(resource, framecount));
'@ @'
		~Resource_DX12()
		{
			std::scoped_lock lck(allocationhandler->destroylocker);
			uint64_t framecount = allocationhandler->framecount;
			if (resource) ARCWickedResourceDestroyed(resource.Get());
			if (allocation) allocationhandler->destroyer_allocations.push_back(std::make_pair(allocation, framecount));
			if (resource) allocationhandler->destroyer_resources.push_back(std::make_pair(resource, framecount));
'@ 'resource destruction'

Replace-Once $dx12 @'
		// Create frame-resident resources:
'@ @'
		ARCWickedDeviceReady(
			device.Get(),
			descriptorheap_res.heap_GPU.Get(),
			descriptorheap_sam.heap_GPU.Get());

		// Create frame-resident resources:
'@ 'device ready'

Replace-Once $dx12 @'
		if (internal_state->resource != nullptr)
		{
			internal_state->gpu_address = internal_state->resource->GetGPUVirtualAddress();
		}

		if (desc->usage == Usage::READBACK)
'@ @'
		if (internal_state->resource != nullptr)
		{
			internal_state->gpu_address = internal_state->resource->GetGPUVirtualAddress();
			ARCWickedResourceCreated(internal_state->resource.Get());
		}

		if (desc->usage == Usage::READBACK)
'@ 'buffer resource creation'

Replace-Once $dx12 @'
		if (texture->desc.usage == Usage::READBACK)
		{
			hr = dx12_check(internal_state->resource->Map(0, nullptr, &texture->mapped_data));
'@ @'
		if (internal_state->resource != nullptr)
		{
			ARCWickedResourceCreated(internal_state->resource.Get());
		}

		if (texture->desc.usage == Usage::READBACK)
		{
			hr = dx12_check(internal_state->resource->Map(0, nullptr, &texture->mapped_data));
'@ 'texture resource creation'

Replace-Once $dx12 @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uav, ID3D12Resource* res)
'@ @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
				ARCWickedObserveSRV(device->descriptorheap_res.heap_GPU.Get(), static_cast<std::uint32_t>(index), res, &srv);
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uav, ID3D12Resource* res)
'@ 'SRV observation'

Replace-Once $dx12 @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_SAMPLER_DESC& sam)
'@ @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
				ARCWickedObserveUAV(device->descriptorheap_res.heap_GPU.Get(), static_cast<std::uint32_t>(index), res, &uav);
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_SAMPLER_DESC& sam)
'@ 'UAV observation'

Replace-Once $dx12 @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_RENDER_TARGET_VIEW_DESC& rtv, ID3D12Resource* res)
'@ @'
				allocationhandler->device->CopyDescriptorsSimple(1, dst_bindless, handle, type);
				ARCWickedObserveSampler(device->descriptorheap_sam.heap_GPU.Get(), static_cast<std::uint32_t>(index));
			}
		}
		void init(const GraphicsDevice_DX12* device, const D3D12_RENDER_TARGET_VIEW_DESC& rtv, ID3D12Resource* res)
'@ 'sampler observation'

Replace-Once $dx12 @'
		if (queue == QUEUE_GRAPHICS)
		{
			D3D12_RECT pRects
'@ @'
		if (queue != QUEUE_VIDEO_DECODE)
		{
			ARCWickedCommandBegin(commandlist.GetGraphicsCommandList(), queues[queue].desc.Type);
		}

		if (queue == QUEUE_GRAPHICS)
		{
			D3D12_RECT pRects
'@ 'command begin'

Replace-Once $dx12 @'
		assert(slot < DESCRIPTORBINDER_SRV_COUNT);
		CommandList_DX12& commandlist = GetCommandList(cmd);
		auto& binder = commandlist.binder;
'@ @'
		assert(slot < DESCRIPTORBINDER_SRV_COUNT);
		CommandList_DX12& commandlist = GetCommandList(cmd);
		if (resource != nullptr && resource->IsValid())
		{
			ARCWickedResourceUse(commandlist.GetGraphicsCommandList(), to_internal(resource)->resource.Get(), false);
		}
		auto& binder = commandlist.binder;
'@ 'SRV resource use'

Replace-Once $dx12 @'
		assert(slot < DESCRIPTORBINDER_UAV_COUNT);
		CommandList_DX12& commandlist = GetCommandList(cmd);
		auto& binder = commandlist.binder;
'@ @'
		assert(slot < DESCRIPTORBINDER_UAV_COUNT);
		CommandList_DX12& commandlist = GetCommandList(cmd);
		if (resource != nullptr && resource->IsValid())
		{
			ARCWickedResourceUse(commandlist.GetGraphicsCommandList(), to_internal(resource)->resource.Get(), true);
		}
		auto& binder = commandlist.binder;
'@ 'UAV resource use'

$drawReplacements = @(
    @('commandlist.GetGraphicsCommandList()->DrawInstanced(vertexCount, 1, startVertexLocation, 0);',
      'commandlist.GetGraphicsCommandList()->DrawInstanced(vertexCount, 1, startVertexLocation, 0);' + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 0);"),
    @('commandlist.GetGraphicsCommandList()->DrawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);',
      'commandlist.GetGraphicsCommandList()->DrawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);' + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 1);"),
    @('commandlist.GetGraphicsCommandList()->DrawInstanced(vertexCount, instanceCount, startVertexLocation, startInstanceLocation);',
      'commandlist.GetGraphicsCommandList()->DrawInstanced(vertexCount, instanceCount, startVertexLocation, startInstanceLocation);' + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 0);"),
    @('commandlist.GetGraphicsCommandList()->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);',
      'commandlist.GetGraphicsCommandList()->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);' + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 1);"),
    @('commandlist.GetGraphicsCommandList()->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);',
      'commandlist.GetGraphicsCommandList()->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);' + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 2);")
)
foreach ($pair in $drawReplacements) {
    Replace-Once $dx12 $pair[0] $pair[1] ('command telemetry: ' + $pair[0])
}

$indirectCalls = @(
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(drawInstancedIndirectCommandSignature.Get(), 1, internal_state->resource.Get(), args_offset, nullptr, 0);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(drawIndexedInstancedIndirectCommandSignature.Get(), 1, internal_state->resource.Get(), args_offset, nullptr, 0);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(pso_internal->drawInstancedIndirectCountCommandSignature.Get(), max_count, args_internal->resource.Get(), args_offset, count_internal->resource.Get(), count_offset);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(pso_internal->drawIndexedInstancedIndirectCountCommandSignature.Get(), max_count, args_internal->resource.Get(), args_offset, count_internal->resource.Get(), count_offset);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(dispatchIndirectCommandSignature.Get(), 1, internal_state->resource.Get(), args_offset, nullptr, 0);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(dispatchMeshIndirectCommandSignature.Get(), 1, internal_state->resource.Get(), args_offset, nullptr, 0);',
 'commandlist.GetGraphicsCommandList()->ExecuteIndirect(pso_internal->dispatchMeshIndirectCountCommandSignature.Get(), max_count, args_internal->resource.Get(), args_offset, count_internal->resource.Get(), count_offset);'
)
foreach ($call in $indirectCalls) {
    Replace-Once $dx12 $call ($call + "`r`n`t`tARCWickedCountCommand(commandlist.GetGraphicsCommandList(), 3);") ('indirect telemetry: ' + $call)
}

Replace-Once $dx12 @'
		queue->ExecuteCommandLists(
			(UINT)submit_cmds.size(),
			submit_cmds.data()
		);
'@ @'
		queue->ExecuteCommandLists(
			(UINT)submit_cmds.size(),
			submit_cmds.data()
		);
		ARCWickedSubmit(queue.Get(), submit_cmds.data(), submit_cmds.size(), desc.Type);
'@ 'queue submission'

Replace-Once $dx12 @'
					HRESULT hr = dx12_check(swapchain_internal->swapChain->Present(swapchain->desc.vsync, presentFlags));

					// If the device was reset
'@ @'
					HRESULT hr = dx12_check(swapchain_internal->swapChain->Present(swapchain->desc.vsync, presentFlags));
					ARCWickedPresent(
						static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(swapchain_internal->swapChain.Get())),
						swapchain->desc.vsync,
						presentFlags,
						hr);

					// If the device was reset
'@ 'present telemetry'

$proj = Join-Path $testsDir 'Tests.vcxproj'
Replace-AllChecked $proj '<LanguageStandard>stdcpp17</LanguageStandard>' '<LanguageStandard>stdcpplatest</LanguageStandard>' 4 'C++ language level'
Replace-Once $proj @'
      <AdditionalIncludeDirectories>$(SolutionDir)WickedEngine;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
'@ @'
      <AdditionalIncludeDirectories>$(SolutionDir)WickedEngine;$(ARC_ROOT)\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
'@ 'Release ARC include path'
Replace-Once $proj @'
      <AdditionalLibraryDirectories>$(SolutionDir)BUILD\$(Platform)\$(Configuration);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <LinkTimeCodeGeneration>UseFastLinkTimeCodeGeneration</LinkTimeCodeGeneration>
'@ @'
      <AdditionalLibraryDirectories>$(SolutionDir)BUILD\$(Platform)\$(Configuration);$(ARC_ROOT)\build-stage14_5\Release;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>arc-dx12-observer.lib;arc-core.lib;d3d12.lib;dxgi.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <LinkTimeCodeGeneration>UseFastLinkTimeCodeGeneration</LinkTimeCodeGeneration>
'@ 'Release ARC libraries'
Replace-Once $proj @'
    <ClCompile Include="Tests.cpp" />
'@ @'
    <ClCompile Include="Tests.cpp" />
    <ClCompile Include="ArcWickedBridge.cpp" />
'@ 'bridge compile item'
Replace-Once $proj @'
    <ClInclude Include="Tests.h" />
'@ @'
    <ClInclude Include="Tests.h" />
    <ClInclude Include="ArcWickedBridge.h" />
'@ 'bridge header item'

Write-Host "Stage 14.5 Wicked Engine overlay applied at pinned SHA $PinnedWickedSha" -ForegroundColor Green
