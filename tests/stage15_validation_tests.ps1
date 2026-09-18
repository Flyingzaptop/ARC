$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/stage15-validation.ps1')
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$pop=[pscustomobject]@{capture_phase='baseline_peak_measured'; capture_scene_index=1; capture_frame=200; alive_resources=140}
$windows=@([pscustomobject]@{captured=$true;live_population=120;frame=100},[pscustomobject]@{captured=$true;live_population=140;frame=200})
Check (Test-SemanticPopulationWitness $pop $windows) 'Measured peak population must have a witness'
$pop.alive_resources=150
Check (-not (Test-SemanticPopulationWitness $pop $windows)) 'Invented larger population must be rejected'
$pop.alive_resources=140; $pop.capture_frame=100
Check (-not (Test-SemanticPopulationWitness $pop $windows)) 'Population from another frame must be rejected'
$record = '{"safe":false,"observe":true,"string":"false"}' | ConvertFrom-Json
$cohortWindows=@(0..4 | ForEach-Object { $start=1+$_*16; [pscustomobject]@{captured=$true;history_complete=$true;active_resources=80;used_resource_ids=@($start..($start+79))} })
$cohort=Get-MeasuredResourceCohort $cohortWindows
Check ($cohort.valid -and $cohort.count -eq 144) 'Measured IDs must be deduplicated across all windows'
$cohortWindows[0].used_resource_ids[1]=1
Check (-not (Get-MeasuredResourceCohort $cohortWindows).valid) 'Duplicates inside a window must fail'
$cohortWindows[0].used_resource_ids[1]=2; $cohortWindows[0].active_resources=81
Check (-not (Get-MeasuredResourceCohort $cohortWindows).valid) 'Claimed activity must match the ID list'
$cohortWindows[0].active_resources=80; $cohortWindows[0].history_complete=$false
Check (-not (Get-MeasuredResourceCohort $cohortWindows).valid) 'Incomplete history must not supply complexity evidence'
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
    graph_errors=0; failed_observations=0; bridge_rejections=0; backend_failures=0; invalid_samples=0; present_failures=0; device_removed_reason=0; vsync_present_count=0
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
$observations=@(1..42 | ForEach-Object { $family=($_-1)%6; [pscustomobject]@{resource=$_; expected_family=$family; predicted_class=@(2,3,5,6,8,9)[$family]; confidence=0.8} })
$audit=[pscustomobject]@{scope='six_resource_use_families'; labels_used_for_inference=$false; fine_subtypes_validated=$false; truncated=$false; metadata_mismatches=0; samples=42; families=6; covered=42;correct=42;coverage=1.0; family_precision=1.0;observations=$observations}
Check (Test-SemanticFamilyAudit $audit) 'Independent family audit must accept valid evidence'
$audit.family_precision=0.89
Check (-not (Test-SemanticFamilyAudit $audit)) 'Incorrect families must fail audit'
$audit.family_precision=1; $audit.truncated=$true
Check (-not (Test-SemanticFamilyAudit $audit)) 'Truncated audit must fail closed'
$audit.truncated=$false; $audit.observations[0].predicted_class=9
Check (-not (Test-SemanticFamilyAudit $audit)) 'Reported accuracy must agree with raw independently scored rows'
