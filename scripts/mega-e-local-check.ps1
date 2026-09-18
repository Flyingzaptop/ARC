param(
    [string]$BuildDirectory = 'build-mega-e',
    [switch]$NoVisual
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo $BuildDirectory

Write-Host '=== Configure Mega E ===' -ForegroundColor Cyan
& cmake -S $repo -B $build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

Write-Host '=== Build Mega E ===' -ForegroundColor Cyan
& cmake --build $build --config Release --target arc-temporal-visibility-tests arc-visual-importance-tests arc-mega-e-scenario-tests arc-gpu-attribution-native arc-temporal-visibility-native arc-mega-e-visual-fps
if ($LASTEXITCODE -ne 0) { throw "Mega E build failed: $LASTEXITCODE" }

Write-Host '=== Stage 18 + 19 deterministic validation ===' -ForegroundColor Cyan
& ctest --test-dir $build -C Release --output-on-failure -R 'arc-(temporal-visibility|visual-importance|mega-e-scenario)-tests'
if ($LASTEXITCODE -ne 0) { throw "Mega E deterministic validation failed: $LASTEXITCODE" }

Write-Host '=== Native D3D12 Stage 18 + attribution foundation smoke ===' -ForegroundColor Cyan
& ctest --test-dir $build -C Release --output-on-failure -R '^(arc-gpu-attribution-native|arc-temporal-visibility-native)$'
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
