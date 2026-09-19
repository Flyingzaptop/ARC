param(
    [string]$BuildDirectory = 'build-mega-e',
    [switch]$NoVisual
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo $BuildDirectory

function Resolve-CMakeTool {
    param([Parameter(Mandatory=$true)][string]$ToolName)

    $command = Get-Command ($ToolName + '.exe') -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installations = & $vswhere -products * -property installationPath 2>$null
        foreach ($installation in $installations) {
            if ([string]::IsNullOrWhiteSpace($installation)) { continue }
            $candidate = Join-Path $installation ('Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\' + $ToolName + '.exe')
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    $roots = @()
    if ($env:ProgramFiles) {
        $roots += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio')
    }
    if (${env:ProgramFiles(x86)}) {
        $roots += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio')
    }

    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($year in @('2022','2019')) {
            $yearRoot = Join-Path $root $year
            if (-not (Test-Path -LiteralPath $yearRoot)) { continue }
            $editions = Get-ChildItem -LiteralPath $yearRoot -Directory -ErrorAction SilentlyContinue
            foreach ($edition in $editions) {
                $candidate = Join-Path $edition.FullName ('Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\' + $ToolName + '.exe')
                if (Test-Path -LiteralPath $candidate) { return $candidate }
            }
        }
    }

    return $null
}

$cmake = Resolve-CMakeTool -ToolName 'cmake'
$ctest = Resolve-CMakeTool -ToolName 'ctest'

if (-not $cmake -or -not $ctest) {
    throw @"
CMake/CTest were not found.

Install the Visual Studio component:
  Desktop development with C++
and ensure:
  C++ CMake tools for Windows
is selected.

Then rerun:
  run-mega-e-local.cmd
"@
}

Write-Host "Using CMake: $cmake" -ForegroundColor DarkGray
Write-Host "Using CTest: $ctest" -ForegroundColor DarkGray

Write-Host '=== Configure Mega E ===' -ForegroundColor Cyan
& $cmake -S $repo -B $build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

Write-Host '=== Build Mega E ===' -ForegroundColor Cyan
& $cmake --build $build --config Release --target arc-temporal-visibility-tests arc-visual-importance-tests arc-mega-e-scenario-tests arc-gpu-attribution-native arc-temporal-visibility-native arc-mega-e-visual-fps
if ($LASTEXITCODE -ne 0) { throw "Mega E build failed: $LASTEXITCODE" }

Write-Host '=== Stage 18 + 19 deterministic validation ===' -ForegroundColor Cyan
& $ctest --test-dir $build -C Release --output-on-failure -R 'arc-(temporal-visibility|visual-importance|mega-e-scenario)-tests'
if ($LASTEXITCODE -ne 0) { throw "Mega E deterministic validation failed: $LASTEXITCODE" }

Write-Host '=== Native D3D12 Stage 18 + attribution foundation smoke ===' -ForegroundColor Cyan
& $ctest --test-dir $build -C Release --output-on-failure -R '^(arc-gpu-attribution-native|arc-temporal-visibility-native)$'
if ($LASTEXITCODE -ne 0) { throw "Native D3D12 Mega E foundation test failed: $LASTEXITCODE" }

Write-Host ''
Write-Host 'Mega E Stage 18 + 19 validation PASS.' -ForegroundColor Green

if (-not $NoVisual) {
    $exe = Join-Path $build 'Release\arc-mega-e-visual-fps.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "Visual debugger missing: $exe" }
    Write-Host 'Launching ARC Mega E visual debugger...' -ForegroundColor Green
    Write-Host 'Controls: WASD, Shift, arrows or hold RMB and move mouse, Esc.' -ForegroundColor DarkGray
    Start-Process -FilePath $exe -WorkingDirectory $repo
}
