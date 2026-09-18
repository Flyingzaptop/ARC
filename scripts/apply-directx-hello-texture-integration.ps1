param(
    [Parameter(Mandatory=$true)][string]$ExternalRoot,
    [Parameter(Mandatory=$true)][string]$ArcRepoRoot
)

$ErrorActionPreference='Stop'
$ExpectedUpstreamSha='213dd4fd4918ea009dd8f35adee1aff1f2ecaba4'

function Replace-Exact {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Old,
        [Parameter(Mandatory=$true)][string]$New
    )
    $text=Get-Content -LiteralPath $Path -Raw
    if(-not$text.Contains($Old)){throw "Expected patch anchor not found in $Path"}
    $text=$text.Replace($Old,$New)
    Set-Content -LiteralPath $Path -Value $text -Encoding utf8
}

$ExternalRoot=(Resolve-Path -LiteralPath $ExternalRoot).Path
$ArcRepoRoot=(Resolve-Path -LiteralPath $ArcRepoRoot).Path
$actual=(& git.exe -C $ExternalRoot rev-parse HEAD).Trim()
if($actual-ne$ExpectedUpstreamSha){throw "DirectX-Graphics-Samples SHA mismatch. Expected $ExpectedUpstreamSha, got $actual"}

$sample=Join-Path $ExternalRoot 'Samples\Desktop\D3D12HelloWorld\src\HelloTexture'
if(-not(Test-Path -LiteralPath $sample)){throw "HelloTexture sample not found: $sample"}

Copy-Item -LiteralPath (Join-Path $ArcRepoRoot 'integrations\directx-hello-texture\ArcExternalBridge.h') -Destination $sample -Force
Copy-Item -LiteralPath (Join-Path $ArcRepoRoot 'integrations\directx-hello-texture\ArcExternalBridge.cpp') -Destination $sample -Force

$header=Join-Path $sample 'D3D12HelloTexture.h'
Replace-Exact $header '#include "DXSample.h"' "#include \"DXSample.h\"\r\n#include \"ArcExternalBridge.h\""
Replace-Exact $header '    ComPtr<ID3D12Resource> m_texture;' "    ComPtr<ID3D12Resource> m_texture;\r\n    std::unique_ptr<ArcExternalBridge> m_arc;"

$cpp=Join-Path $sample 'D3D12HelloTexture.cpp'
Replace-Exact $cpp @'
        CD3DX12_ROOT_PARAMETER1 rootParameters[1];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
'@ @'
        CD3DX12_ROOT_PARAMETER1 rootParameters[2];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        rootParameters[1].InitAsConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
'@

Replace-Exact $cpp @'
        Vertex triangleVertices[] =
        {
            { { 0.0f, 0.25f * m_aspectRatio, 0.0f }, { 0.5f, 0.0f } },
            { { 0.25f, -0.25f * m_aspectRatio, 0.0f }, { 1.0f, 1.0f } },
            { { -0.25f, -0.25f * m_aspectRatio, 0.0f }, { 0.0f, 1.0f } }
        };
'@ @'
        Vertex triangleVertices[] =
        {
            { { 0.0f, 1.5f, 0.0f }, { 0.5f, 0.0f } },
            { { 1.5f, -1.5f, 0.0f }, { 1.0f, 1.0f } },
            { { -1.5f, -1.5f, 0.0f }, { 0.0f, 1.0f } }
        };
'@

Replace-Exact $cpp @'
        WaitForPreviousFrame();
    }
}

// Generate a simple black and white checkerboard texture.
'@ @'
        WaitForPreviousFrame();
    }

    m_arc = std::make_unique<ArcExternalBridge>(
        m_device.Get(),
        m_commandQueue.Get(),
        m_commandList.Get(),
        m_fence.Get(),
        m_rtvHeap.Get(),
        m_srvHeap.Get(),
        m_renderTargets,
        FrameCount,
        m_vertexBuffer.Get(),
        m_texture.Get());
}

// Generate a simple black and white checkerboard texture.
'@

Replace-Exact $cpp @'
void D3D12HelloTexture::OnRender()
{
    // Record all the commands we need to render the scene into the command list.
    PopulateCommandList();

    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Present the frame.
    ThrowIfFailed(m_swapChain->Present(1, 0));

    WaitForPreviousFrame();
}
'@ @'
void D3D12HelloTexture::OnRender()
{
    const auto arcFrameStart = std::chrono::steady_clock::now();

    // Record all the commands we need to render the scene into the command list.
    PopulateCommandList();

    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    if (m_arc) m_arc->OnSubmit();

    // Present uncapped so ARC observes renderer/GPU pressure rather than v-sync.
    const HRESULT presentResult = m_swapChain->Present(0, 0);
    if (m_arc) m_arc->OnPresent(presentResult);
    ThrowIfFailed(presentResult);

    WaitForPreviousFrame();

    if (m_arc)
    {
        const double frameMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - arcFrameStart).count();
        m_arc->OnFrame(frameMs);
        if (m_arc->ShouldExit()) PostMessage(Win32Application::GetHwnd(), WM_CLOSE, 0, 0);
    }
}
'@

Replace-Exact $cpp @'
    WaitForPreviousFrame();

    CloseHandle(m_fenceEvent);
'@ @'
    WaitForPreviousFrame();

    if (m_arc) m_arc->Finalize();
    CloseHandle(m_fenceEvent);
'@

Replace-Exact $cpp @'
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    // Set necessary state.
'@ @'
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));
    if (m_arc) m_arc->OnCommandReset();

    // Set necessary state.
'@

Replace-Exact $cpp @'
    m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->RSSetViewports(1, &m_viewport);
'@ @'
    m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRoot32BitConstant(1, m_arc ? m_arc->SampleIterations() : 96u, 0);
    m_commandList->RSSetViewports(1, &m_viewport);
'@

Replace-Exact $cpp @'
    // Indicate that the back buffer will be used as a render target.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
'@ @'
    // Indicate that the back buffer will be used as a render target.
    auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);
    if (m_arc) m_arc->ObserveTransition(m_renderTargets[m_frameIndex].Get(), toRenderTarget);
'@

Replace-Exact $cpp @'
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // Indicate that the back buffer will now be used to present.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(m_commandList->Close());
'@ @'
    m_commandList->DrawInstanced(3, 1, 0, 0);
    if (m_arc) m_arc->ObserveFrameUses(
        m_renderTargets[m_frameIndex].Get(),
        m_vertexBuffer.Get(),
        m_texture.Get());

    // Indicate that the back buffer will now be used to present.
    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
    if (m_arc) m_arc->ObserveTransition(m_renderTargets[m_frameIndex].Get(), toPresent);

    ThrowIfFailed(m_commandList->Close());
    if (m_arc) m_arc->OnCommandClosed();
'@

Replace-Exact $cpp @'
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;
'@ @'
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    if (m_arc) m_arc->OnSignal(fence);
    m_fenceValue++;
'@

Replace-Exact $cpp @'
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
'@ @'
    }

    if (m_arc) m_arc->OnCompletion();
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
'@

$shader=Join-Path $sample 'shaders.hlsl'
Replace-Exact $shader @'
SamplerState g_sampler : register(s0);
'@ @'
SamplerState g_sampler : register(s0);

cbuffer ArcQuality : register(b0)
{
    uint g_arcIterations;
};
'@
Replace-Exact $shader @'
float4 PSMain(PSInput input) : SV_TARGET
{
    return g_texture.Sample(g_sampler, input.uv);
}
'@ @'
float4 PSMain(PSInput input) : SV_TARGET
{
    const uint count = max(g_arcIterations, 1u);
    float4 color = 0.0;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const float t = (float(i) + 0.5) / float(count);
        const float2 jitter = (float2(frac(t * 7.0), frac(t * 13.0)) - 0.5) / 256.0;
        color += g_texture.Sample(g_sampler, saturate(input.uv + jitter));
    }
    return color / float(count);
}
'@

$main=Join-Path $sample 'Main.cpp'
Replace-Exact $main '    D3D12HelloTexture sample(1280, 720, L"D3D12 Hello Texture");' '    D3D12HelloTexture sample(1920, 1080, L"D3D12 Hello Texture + ARC External Acceptance");'

$project=Join-Path $sample 'D3D12HelloTexture.vcxproj'
Replace-Exact $project '    <ClInclude Include="Win32Application.h" />' "    <ClInclude Include=\"ArcExternalBridge.h\" />\r\n    <ClInclude Include=\"Win32Application.h\" />"
Replace-Exact $project '    <ClCompile Include="Win32Application.cpp" />' "    <ClCompile Include=\"ArcExternalBridge.cpp\" />\r\n    <ClCompile Include=\"Win32Application.cpp\" />"
Replace-Exact $project @'
  <Import Project="$(VCTargetsPath)Microsoft.Cpp.targets" />
'@ @'
  <ItemDefinitionGroup Condition="'$(Platform)'=='x64'">
    <ClCompile>
      <AdditionalIncludeDirectories>$(ArcRepoRoot)\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <LanguageStandard>stdcpplatest</LanguageStandard>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories>$(ArcRepoRoot)\build\Release;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>arc-dx12-observer.lib;arc-core.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <Import Project="$(VCTargetsPath)Microsoft.Cpp.targets" />
'@

Write-Host "ARC external renderer overlay applied to Microsoft D3D12HelloTexture at $actual" -ForegroundColor Green
