param(
    [int]$Rounds = 6,
    [int]$Objects = 24,
    [int]$ObjectMiB = 2,
    [switch]$SkipDebugBuild
)
$ErrorActionPreference = 'Stop'
Write-Host '=== ARC Stage 2A.1 autonomous residency validation ==='
Write-Host 'Release: GPU + D3D12 debug layer + adversarial stress tests'
& "$PSScriptRoot/validate.ps1" -Configuration Release -GpuTests -DebugLayer -StressTests -Clean
if ($LASTEXITCODE -ne 0) { throw "Release validation failed: $LASTEXITCODE" }
if (-not $SkipDebugBuild) {
    Write-Host 'Debug: GPU + D3D12 debug layer'
    & "$PSScriptRoot/validate.ps1" -Configuration Debug -GpuTests -DebugLayer -Clean
    if ($LASTEXITCODE -ne 0) { throw "Debug validation failed: $LASTEXITCODE" }
}
Write-Host 'Rotated baseline/oracle/autonomous residency benchmark'
& "$PSScriptRoot/residency-benchmark.ps1" -Rounds $Rounds -Objects $Objects -ObjectMiB $ObjectMiB
if ($LASTEXITCODE -ne 0) { throw "Residency benchmark failed: $LASTEXITCODE" }
$benchmark = 'traces/residency-benchmark.json'
$summary = 'traces/residency-benchmark-summary.json'
if (-not (Test-Path $benchmark) -or -not (Test-Path $summary)) { throw 'Expected benchmark output files were not produced' }
Write-Host ''
Write-Host 'Stage 2A.1 validation complete.'
Write-Host "Send these files for review:"
Write-Host "  $benchmark"
Write-Host "  $summary"
