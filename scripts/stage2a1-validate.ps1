param(
    [int]$Rounds = 6,
    [int]$Objects = 24,
    [int]$ObjectMiB = 2,
    [switch]$SkipDebugBuild
)
$ErrorActionPreference = 'Stop'

Write-Host '=== ARC combined residency + tiled texture validation ==='
Write-Host 'Release: GPU + D3D12 debug layer + adversarial stress tests'
& "$PSScriptRoot/validate.ps1" -Configuration Release -GpuTests -DebugLayer -StressTests -Clean
if ($LASTEXITCODE -ne 0) { throw "Release validation failed: $LASTEXITCODE" }

$tiled = 'traces/tiled-texture-lab.json'
if (-not (Test-Path $tiled)) { throw 'Tiled texture GPU test ran but did not produce traces/tiled-texture-lab.json' }
$tiledResult = Get-Content -Raw -LiteralPath $tiled | ConvertFrom-Json
if (-not $tiledResult.initial_contents_verified -or -not $tiledResult.lower_mip_preserved -or -not $tiledResult.promotion_contents_verified) {
    throw 'Tiled texture content validation failed'
}
if ($tiledResult.debug_error_count -ne 0) { throw "Tiled texture debug layer errors: $($tiledResult.debug_error_count)" }

if (-not $SkipDebugBuild) {
    Write-Host 'Debug: GPU + D3D12 debug layer'
    & "$PSScriptRoot/validate.ps1" -Configuration Debug -GpuTests -DebugLayer -Clean
    if ($LASTEXITCODE -ne 0) { throw "Debug validation failed: $LASTEXITCODE" }
}

Write-Host 'Observer baseline/light/full regression benchmark'
& "$PSScriptRoot/benchmark.ps1" -Iterations 3000 -Rounds 3
if ($LASTEXITCODE -ne 0) { throw "Observer benchmark failed: $LASTEXITCODE" }

Write-Host 'Rotated baseline/oracle/autonomous residency benchmark'
& "$PSScriptRoot/residency-benchmark.ps1" -Rounds $Rounds -Objects $Objects -ObjectMiB $ObjectMiB
if ($LASTEXITCODE -ne 0) { throw "Residency benchmark failed: $LASTEXITCODE" }

Write-Host 'Multi-workload memory/latency frontier benchmark'
$frontierRounds = [Math]::Max(2, [Math]::Min(3, [int][Math]::Ceiling($Rounds / 3.0)))
& "$PSScriptRoot/residency-frontier.ps1" -Rounds $frontierRounds -Objects $Objects -ObjectMiB $ObjectMiB -WarmupEpochs 300 -MeasuredEpochs 1200
if ($LASTEXITCODE -ne 0) { throw "Residency frontier failed: $LASTEXITCODE" }

$expected = @(
    'traces/benchmark-matrix.json',
    'traces/benchmark-summary.json',
    'traces/residency-benchmark.json',
    'traces/residency-benchmark-summary.json',
    'traces/residency-frontier.json',
    'traces/residency-frontier-summary.json',
    'traces/residency-frontier.csv',
    'traces/tiled-texture-lab.json'
)
foreach ($path in $expected) {
    if (-not (Test-Path $path)) { throw "Expected validation artifact was not produced: $path" }
}

Write-Host ''
Write-Host 'Combined validation complete.'
Write-Host 'Primary review artifacts:'
foreach ($path in $expected) { Write-Host "  $path" }
