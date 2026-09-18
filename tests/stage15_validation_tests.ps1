$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/stage15-validation.ps1')
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$record = '{"safe":false,"observe":true,"string":"false"}' | ConvertFrom-Json
Check (Test-ExplicitBoolean $record safe $false) 'Explicit false must pass'
Check (Test-ExplicitBoolean $record observe $true) 'Explicit true must pass'
Check (-not (Test-ExplicitBoolean $record missing $false)) 'Missing false flag must fail closed'
Check (-not (Test-ExplicitBoolean $record string $false)) 'String flag must fail'
Check (-not (Test-ExplicitBoolean $null safe $false)) 'Missing block must fail'
$capture = '{"captured":true,"history_complete":false}' | ConvertFrom-Json
Check (-not (Test-ExplicitBoolean $capture history_complete $true)) 'Pruned capture must fail'
Check (-not (Test-ExplicitBoolean $record history_complete $true)) 'Missing history declaration must fail'
$valid = '[[0.1,0.7],[0.8,0.2]]' | ConvertFrom-Json
Check (Test-DistanceMatrix $valid 2) 'Valid distances must pass'
foreach ($bad in @($null, '0.2', $true, [double]::NaN, [double]::PositiveInfinity, -0.01, 1.01)) {
    $matrix = @(@(0.1, $bad), @(0.8, 0.2))
    Check (-not (Test-DistanceMatrix $matrix 2)) 'Invalid distance must fail'
}
Check (-not (Test-DistanceMatrix @(@(0.1), @(0.8, 0.2)) 2)) 'Ragged matrix must fail'
Check (-not (Test-FiniteNumber $null 0 1)) 'Missing score must fail'
$comparison = [pscustomobject]@{
    valid=$true; acceptance_evaluated=$false; hook_timings=[pscustomobject]@{enabled=$false}
    mode='off'; arc_source_sha='test'; experiment='three_arm_comparison'; scene_offset=0; width=1920; height=1080
    graph_errors=0; failed_observations=0; bridge_rejections=0; backend_failures=0; invalid_samples=0; present_failures=0; device_removed_reason=0
    calibration_p40_ms=2.0; calibration_p50_ms=3.0
    scenes=@('model','shadows','water','volumetric','instances_65k') | ForEach-Object {
        [pscustomobject]@{name=$_; gpu_samples=50; cpu_samples=50; gpu_p50_ms=2.0; gpu_p95_ms=3.0; cpu_p50_ms=4.0; cpu_p95_ms=5.0}
    }
}
Check (Test-WickedComparisonRun $comparison off test 0) 'Valid comparison must pass'
$comparison.scenes[0].gpu_p50_ms = [double]::NaN
Check (-not (Test-WickedComparisonRun $comparison off test 0)) 'NaN timing must fail'
$comparison.scenes[0].gpu_p50_ms = 2.0
$comparison.scenes[0].name = 'unknown'
Check (-not (Test-WickedComparisonRun $comparison off test 0)) 'Wrong scene must fail'
$comparison.scenes[0].name = 'model'
Check (-not (Test-WickedComparisonRun $comparison off test 1)) 'Wrong scene order must fail'
$comparison.backend_failures = 1
Check (-not (Test-WickedComparisonRun $comparison off test 0)) 'Backend failure must invalidate comparison'
Write-Host 'stage15-validation-tests: PASS'
