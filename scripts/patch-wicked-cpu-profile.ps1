param([Parameter(Mandatory=$true)][string]$WickedRoot)
$ErrorActionPreference = 'Stop'
function Replace-One([string]$Text, [string]$Old, [string]$New) {
    $Old=$Old.Replace("`r`n","`n"); $New=$New.Replace("`r`n","`n")
    $index=$Text.IndexOf($Old,[StringComparison]::Ordinal)
    if ($index -lt 0 -or $Text.IndexOf($Old,$index+$Old.Length,[StringComparison]::Ordinal) -ge 0) { throw "CPU profile anchor missing/ambiguous: $Old" }
    return $Text.Substring(0,$index)+$New+$Text.Substring($index+$Old.Length)
}
function Read-Source([string]$Relative) { return [IO.File]::ReadAllText((Join-Path $WickedRoot $Relative)).Replace("`r`n","`n") }
function Write-Source([string]$Relative,[string]$Text) { [IO.File]::WriteAllText((Join-Path $WickedRoot $Relative),$Text,[Text.UTF8Encoding]::new($false)) }

$path='WickedEngine/wiProfiler.cpp'; $text=Read-Source $path
$text=Replace-One $text '#include "wiProfiler.h"' "#include `"wiProfiler.h`"`n#include `"ArcWickedHooks.h`""
$old='range.times[range.avg_counter++ % arraysize(range.times)] = range.time;'
$text=Replace-One $text $old "if (range.IsCPURange()) ARCWickedCpuSample(range.name.c_str(), range.time);`n`t`t`t$old"
Write-Source $path $text

$path='WickedEngine/wiApplication.cpp'; $text=Read-Source $path
$text=Replace-One $text '#include "wiApplication.h"' "#include `"wiApplication.h`"`n#include `"ArcWickedHooks.h`""
$text=Replace-One $text 'wi::helper::Sleep(10);' 'wi::Timer arcInactive; wi::helper::Sleep(10); ARCWickedCpuSample("Inactive sleep", arcInactive.elapsed_milliseconds());'
$text=Replace-One $text "`t`tUpdate(deltaTime);" "`t`twi::Timer arcUpdate; Update(deltaTime); ARCWickedCpuSample(`"Application Update`", arcUpdate.elapsed_milliseconds());"
$text=Replace-One $text "`t`tRender();" "`t`twi::Timer arcRender; Render(); ARCWickedCpuSample(`"Application Render`", arcRender.elapsed_milliseconds());"
$text=Replace-One $text "`t`tCompose(cmd);" "`t`twi::Timer arcCompose; Compose(cmd); ARCWickedCpuSample(`"Application Compose`", arcCompose.elapsed_milliseconds());"
$old="wi::profiler::EndFrame(cmd);`n`t`tgraphicsDevice->SubmitCommandLists();"
$text=Replace-One $text $old "wi::profiler::EndFrame(cmd);`n`t`twi::Timer arcSubmit; graphicsDevice->SubmitCommandLists(); ARCWickedCpuSample(`"SubmitCommandLists`", arcSubmit.elapsed_milliseconds());"
Write-Source $path $text

$path='Samples/Tests/Tests.cpp'; $text=Read-Source $path
$text=Replace-One $text '#include "ArcWickedBridge.h"' "#include `"ArcWickedBridge.h`"`n#include `"ArcWickedHooks.h`""
$text=Replace-One $text '    RenderPath3D::Update(dt);' '    wi::Timer arcScene; RenderPath3D::Update(dt); ARCWickedCpuSample("RenderPath3D Update", arcScene.elapsed_milliseconds());'
Write-Source $path $text

$path='Samples/Tests/main_Windows.cpp'; $text=Read-Source $path
$text=Replace-One $text '#include "stdafx.h"' "#include `"stdafx.h`"`n#include `"ArcWickedBridge.h`""
$text=Replace-One $text 'tests.Run();' 'arc_wicked::PollCpuProfile(); if (!arc_wicked::Finished()) tests.Run();'
Write-Source $path $text
