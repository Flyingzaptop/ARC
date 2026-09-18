$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$bridge=[IO.File]::ReadAllText((Join-Path $root 'integrations/wicked-engine/ArcWickedBridge.cpp'))
# On the pinned renderer this event calls WaitForGPU and ResizeBuffers even
# when the value is unchanged. It must not belong to the frame/scene harness.
if ($bridge -match '\bSetVSync\s*\(') { throw 'Wicked runtime harness must not broadcast the swapchain-recreating SetVSync event.' }
$overlay=[IO.File]::ReadAllText((Join-Path $root 'scripts/apply-wicked-engine-integration.ps1'))
if ($overlay -notmatch 'tests\.swapChain\.desc\.vsync = false;') { throw 'Native swapchain VSync must be configured before SetWindow.' }
$windowBlock=[regex]::Match($overlay,"(?s)\`$windowNew = @'.*?'@").Value
if (-not $windowBlock -or $windowBlock.IndexOf('tests.swapChain.desc.vsync = false;') -lt 0 -or
    $windowBlock.IndexOf('tests.swapChain.desc.vsync = false;') -gt $windowBlock.IndexOf('HWND hWnd = CreateWindowW')) { throw 'Presentation policy must precede CreateWindow, which can send WM_SIZE synchronously.' }
Write-Host 'wicked-presentation-contract-tests: PASS'
if (-not $overlay.Contains('Unexpected scene presentation override')) { throw 'Patched scene callbacks must reject presentation overrides.' }
if (-not $bridge.Contains('if (sync_interval) g_vsync_presents.fetch_add')) { throw 'Actual Present sync intervals must be checked, including OFF.' }
