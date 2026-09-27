param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere unavailable' }
$installation = & $vswhere -latest -products '*' -property installationPath
if (-not $installation) { throw 'Visual Studio Build Tools unavailable' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw 'vcvars64.bat unavailable' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$source = $PSScriptRoot
$object = Join-Path $OutputDirectory 'context.obj'
$cppObject = Join-Path $OutputDirectory 'replay.obj'
$exe = Join-Path $OutputDirectory 'replay.exe'
$assemble = 'ml64 /nologo /c /Fo"' + $object + '" "' +
            (Join-Path $source 'context.asm') + '"'
$compile = 'cl /nologo /std:c++17 /EHsc /O2 /W4 /Fo:"' + $cppObject +
           '" /Fe:"' + $exe +
           '" "' + (Join-Path $source 'replay.cpp') + '" "' + $object +
           '" /link /DYNAMICBASE:NO /FIXED /BASE:0x180000000'
$command = '"' + $vcvars + '" >nul && ' + $assemble + ' && ' + $compile
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "native build failed: $LASTEXITCODE" }
Write-Output $exe
