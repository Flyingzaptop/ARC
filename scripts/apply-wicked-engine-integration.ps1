param(
    [Parameter(Mandatory=$true)][string]$WickedRoot,
    [Parameter(Mandatory=$true)][string]$ArcRoot,
    [string]$ArcBuildRoot = ""
)

$ErrorActionPreference = 'Stop'
$ExpectedWickedSha = '0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7'
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

$actual = (& git -C $WickedRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actual -ne $ExpectedWickedSha) {
    throw "Wicked Engine source mismatch. Expected $ExpectedWickedSha, got $actual"
}
if ([string]::IsNullOrWhiteSpace($ArcBuildRoot)) {
    $ArcBuildRoot = Join-Path $ArcRoot 'build-wicked'
}

Copy-Item (Join-Path $ArcRoot 'integrations\wicked-engine\ArcWickedHooks.h') (Join-Path $WickedRoot 'WickedEngine\ArcWickedHooks.h') -Force
Copy-Item (Join-Path $ArcRoot 'integrations\wicked-engine\ArcWickedBridge.h') (Join-Path $WickedRoot 'Samples\Tests\ArcWickedBridge.h') -Force
Copy-Item (Join-Path $ArcRoot 'integrations\wicked-engine\ArcWickedBridge.cpp') (Join-Path $WickedRoot 'Samples\Tests\ArcWickedBridge.cpp') -Force

& (Join-Path $ArcRoot 'scripts\patch-wicked-dx12.ps1') -WickedRoot $WickedRoot
if ($LASTEXITCODE -ne 0) { throw 'DX12 overlay failed.' }

# Expose raw GPU-frame timing from Wicked's own DX12 timestamp profiler:
$profHPath = Join-Path $WickedRoot 'WickedEngine\wiProfiler.h'
$ph = Read-Lf $profHPath
$ph = Replace-Once $ph ($T+'bool IsEnabled();') ($T+'bool IsEnabled();'+$LF+$T+'float GetLastGPUFrameTimeMS();') 'profiler accessor declaration'
Write-Utf8Lf $profHPath $ph

$profCppPath = Join-Path $WickedRoot 'WickedEngine\wiProfiler.cpp'
$pc = Read-Lf $profCppPath
$pc = Replace-Once $pc ($T+'std::atomic<uint32_t> nextQuery{ 0 };') ($T+'std::atomic<uint32_t> nextQuery{ 0 };'+$LF+$T+'std::atomic<float> arc_last_gpu_frame_time_ms{ 0.0f };') 'profiler timestamp storage'

$timeGuard = @'
					if (range.time > 1000000.0f)
					{
						// can happen in Apple TBDR GPU, if no pixel was drawn, timestamp becomes invalid and produce huge values
						range.time = 0;
					}
'@
$timeNew = $timeGuard + $LF + $T+$T+$T+$T+$T+'if (range.name == "GPU Frame") arc_last_gpu_frame_time_ms.store(range.time, std::memory_order_relaxed);'
$pc = Replace-Once $pc $timeGuard $timeNew 'profiler raw capture'

$endFrameMarker = $T+'void EndFrame(CommandList cmd)'
$accessor =
    $T+'float GetLastGPUFrameTimeMS()'+$LF+
    $T+'{'+$LF+
    $T+$T+'return arc_last_gpu_frame_time_ms.load(std::memory_order_relaxed);'+$LF+
    $T+'}'+$LF
$pc = Replace-Once $pc $endFrameMarker ($accessor + $endFrameMarker) 'profiler accessor implementation'
Write-Utf8Lf $profCppPath $pc

# Tests harness:
$testsPath = Join-Path $WickedRoot 'Samples\Tests\Tests.cpp'
$tt = Read-Lf $testsPath
$tt = Replace-Once $tt '#include "stdafx.h"' ('#include "stdafx.h"'+$LF+'#include "ArcWickedBridge.h"') 'Tests bridge include'
$loadOld = 'void TestsRenderer::Load()'+$LF+'{'+$LF+$T+'setSSREnabled(false);'
$loadNew = 'void TestsRenderer::Load()'+$LF+'{'+$LF+$T+'arc_wicked::ForceFullQuality();'+$LF+$T+'setSSREnabled(false);'
$tt = Replace-Once $tt $loadOld $loadNew 'Tests Load quality reset'
$updateOld = 'void TestsRenderer::Update(float dt)'+$LF+'{'+$LF+$T+'int selected = testSelector.GetSelected();'
$updateNew = 'void TestsRenderer::Update(float dt)'+$LF+'{'+$LF+
    $T+'resolutionScale = 1.0f;'+$LF+
    $T+'setFSREnabled(false);'+$LF+
    $T+'setFSR2Enabled(false);'+$LF+
    $T+'arc_wicked::HarnessUpdate(testSelector, GetPhysicalWidth(), GetPhysicalHeight());'+$LF+
    $T+'int selected = testSelector.GetSelected();'
$tt = Replace-Once $tt $updateOld $updateNew 'Tests Update harness'
Write-Utf8Lf $testsPath $tt

# Exact 1920x1080 client area:
$mainPath = Join-Path $WickedRoot 'Samples\Tests\main_Windows.cpp'
$mw = Read-Lf $mainPath
$mw = Replace-Once $mw '    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_WICKEDENGINETESTS);' '    wcex.lpszMenuName   = nullptr;' 'remove class menu for borderless harness'
$windowOld = @'
   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
'@
$windowNew = @'
   // Borderless window: the client area itself is exactly 1920x1080 physical
   // pixels. A decorated 1920x1080 window is clamped to the desktop work area
   // on a 1080p display, which previously produced a 1920x1030 client.
   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_POPUP,
      0, 0, 1920, 1080, nullptr, nullptr, hInstance, nullptr);
'@
$mw = Replace-Once $mw $windowOld $windowNew '1080p client window'
$mw = Replace-Once $mw '   ShowWindow(hWnd, nCmdShow);' '   ShowWindow(hWnd, SW_SHOWNORMAL);' 'stable borderless show state'
Write-Utf8Lf $mainPath $mw

# MSBuild wiring:
$projPath = Join-Path $WickedRoot 'Samples\Tests\Tests.vcxproj'
$proj = Read-Lf $projPath
$includeOld = '$(SolutionDir)WickedEngine;%(AdditionalIncludeDirectories)'
$includeNew = '$(SolutionDir)WickedEngine;$(ArcRepoRoot)\include;%(AdditionalIncludeDirectories)'
if (-not $proj.Contains($includeOld)) { throw 'Tests project include path anchor missing.' }
$proj = $proj.Replace($includeOld, $includeNew)
$proj = $proj.Replace('<LanguageStandard>stdcpp17</LanguageStandard>', '<LanguageStandard>stdcpplatest</LanguageStandard>')

$libOld = '$(SolutionDir)BUILD\$(Platform)\$(Configuration);%(AdditionalLibraryDirectories)'
$libNew = '$(SolutionDir)BUILD\$(Platform)\$(Configuration);$(ArcBuildRoot)\Release;%(AdditionalLibraryDirectories)'
if (-not $proj.Contains($libOld)) { throw 'Tests project library path anchor missing.' }
$proj = $proj.Replace($libOld, $libNew)

$libLine = '      <AdditionalLibraryDirectories>'+ $libNew +'</AdditionalLibraryDirectories>'
$depsLine = '      <AdditionalDependencies>arc-core.lib;arc-dx12-observer.lib;dxgi.lib;d3d12.lib;%(AdditionalDependencies)</AdditionalDependencies>'
$proj = $proj.Replace($libLine, $libLine+$LF+$depsLine)

$proj = Replace-Once $proj '    <ClInclude Include="Tests.h" />' ('    <ClInclude Include="Tests.h" />'+$LF+'    <ClInclude Include="ArcWickedBridge.h" />') 'bridge header project item'
$proj = Replace-Once $proj '    <ClCompile Include="Tests.cpp" />' ('    <ClCompile Include="Tests.cpp" />'+$LF+'    <ClCompile Include="ArcWickedBridge.cpp" />') 'bridge cpp project item'
Write-Utf8Lf $projPath $proj

Write-Host 'ARC Wicked Engine overlay applied.'
Write-Host "Wicked SHA: $actual"
Write-Host "ARC root: $ArcRoot"
Write-Host "ARC build: $ArcBuildRoot"
