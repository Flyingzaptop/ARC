param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug', [switch]$GpuTests, [switch]$DebugLayer)
$ErrorActionPreference = 'Stop'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC Build Tools not found' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$ninja = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$buildDirectory = "build/$Configuration"
# CMake configures the installed compiler's include diagnostic prefix.
$gpu = if ($GpuTests) { 'ON' } else { 'OFF' }
$command = "call `"$vcvars`" >nul && set VSLANG=1033&& `"$cmake`" -S . -B $buildDirectory -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DARC_GPU_TESTS=$gpu -DARC_MSVC_INCLUDE_PREFIX=`"Примечание: включение файла:`" -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build $buildDirectory && `"$ctest`" --test-dir $buildDirectory --output-on-failure --timeout 60"
if ($DebugLayer) { $command = 'set ARC_D3D12_DEBUG=1&& ' + $command }
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Validation failed: $LASTEXITCODE" }
