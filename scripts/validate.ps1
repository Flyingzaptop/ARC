param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug', [switch]$GpuTests, [switch]$DebugLayer, [switch]$StressTests, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC Build Tools not found' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$ninja = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$buildDirectory = "build/$Configuration"
# VSLANG=1033 forces English MSVC diagnostics. Keep this argument ASCII-only because
# Windows PowerShell 5.1 treats BOM-less UTF-8 scripts as the active ANSI code page.
$gpu = if ($GpuTests) { 'ON' } else { 'OFF' }
$stress = if ($StressTests) { 'ON' } else { 'OFF' }
$timeout = if ($StressTests) { 300 } else { 60 }
$cleanArgument = if ($Clean) { ' --clean-first' } else { '' }
$command = "call `"$vcvars`" >nul && set VSLANG=1033&& `"$cmake`" -S . -B $buildDirectory -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DARC_GPU_TESTS=$gpu -DARC_STRESS_TESTS=$stress -DARC_MSVC_INCLUDE_PREFIX=`"Note: including file:`" -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build $buildDirectory$cleanArgument && `"$ctest`" --test-dir $buildDirectory --output-on-failure --timeout $timeout"
if ($DebugLayer) { $command = 'set ARC_D3D12_DEBUG=1&& ' + $command }
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Validation failed: $LASTEXITCODE" }
