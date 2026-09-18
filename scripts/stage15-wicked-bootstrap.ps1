param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/mega-c-stage15-scene-understanding',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-MegaC-Stage15",
    [string]$ExpectedSha = '',
    [ValidateRange(10,120)][int]$Seconds = 30
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'stage15-validation.ps1')

function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Mean-Value($Values) {
    $items = @($Values)
    if ($items.Count -eq 0) { return 0.0 }
    return [double](($items | Measure-Object -Average).Average)
}

$arc = Join-Path $WorkRoot 'arc'
$archiveRoot = Join-Path $WorkRoot 'stage15-run-archive'
New-Item -ItemType Directory -Force $WorkRoot,$archiveRoot | Out-Null

# Preserve any prior local run before git clean removes untracked diagnostics.
$priorLocalRoot = Join-Path $arc 'results\stage14_5-wicked-local'
if (Test-Path -LiteralPath $priorLocalRoot) {
    foreach ($prior in Get-ChildItem -LiteralPath $priorLocalRoot -Directory -ErrorAction SilentlyContinue) {
        $destination = Join-Path $archiveRoot $prior.Name
        if (-not (Test-Path -LiteralPath $destination)) {
            Copy-Item -LiteralPath $prior.FullName -Destination $destination -Recurse -Force
        }
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $arc '.git'))) {
    & git.exe clone $RepoUrl $arc
    if ($LASTEXITCODE -ne 0) { throw "ARC clone failed: $LASTEXITCODE" }
}
& git.exe -C $arc reset --hard
& git.exe -C $arc clean -fdx
& git.exe -C $arc fetch origin $Branch --prune
if ($LASTEXITCODE -ne 0) { throw "ARC fetch failed: $LASTEXITCODE" }
& git.exe -C $arc switch -C $Branch ("origin/" + $Branch)
if ($LASTEXITCODE -ne 0) { throw "ARC switch failed: $LASTEXITCODE" }

$sourceSha = (& git.exe -C $arc rev-parse HEAD).Trim()
if ($ExpectedSha -and $sourceSha -ne $ExpectedSha) {
    throw "ARC SHA mismatch. Expected $ExpectedSha, got $sourceSha"
}

$stage14 = Join-Path $arc 'scripts\stage14_5-wicked-bootstrap.ps1'
$stage14Log = Join-Path $archiveRoot ('bootstrap-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
Start-Transcript -Path $stage14Log -Force | Out-Null
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stage14 -RepoUrl $RepoUrl -Branch $Branch -WorkRoot $WorkRoot -ExpectedSha $sourceSha -Seconds $Seconds -NoPublish
    $stage14Exit = $LASTEXITCODE
} finally {
    Stop-Transcript | Out-Null
}

$localRoot = Join-Path $arc 'results\stage14_5-wicked-local'
$run = Get-ChildItem -LiteralPath $localRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'wicked-engine.json') } |
    Select-Object -First 1
if (-not $run) {
    if (Test-Path -LiteralPath $localRoot) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        Copy-Item -LiteralPath $localRoot -Destination (Join-Path $archiveRoot ("incomplete-" + $stamp)) -Recurse -Force
    }
    throw "Stage 14.5 exited $stage14Exit without a local Wicked result. Build/run log: $stage14Log. Preserved diagnostics under $archiveRoot"
}

$archiveRun = Join-Path $archiveRoot $run.Name
if (Test-Path -LiteralPath $archiveRun) { Remove-Item -LiteralPath $archiveRun -Recurse -Force }
Copy-Item -LiteralPath $run.FullName -Destination $archiveRun -Recurse -Force

$raw = Get-Content -LiteralPath (Join-Path $run.FullName 'wicked-engine.json') -Raw | ConvertFrom-Json
$sem = $raw.stage15_semantics
$scene = $raw.scene_understanding

$truthScenes = @($raw.scenes)
$sceneCount = $truthScenes.Count
$truthLabels = @($truthScenes | ForEach-Object { [string]$_.truth_label })
$truthLabelsUnique = $sceneCount -gt 1 -and @($truthLabels | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0 -and @($truthLabels | Sort-Object -Unique).Count -eq $sceneCount

$baseline = @()
$adaptive = @()
$matrix = @()
if ($null -ne $scene) {
    $baseline = @($scene.baseline)
    $adaptive = @($scene.adaptive)
    $matrix = @($scene.baseline_to_adaptive_distance)
}

$capturesPresent =
    $sceneCount -gt 0 -and
    $baseline.Count -eq $sceneCount -and
    $adaptive.Count -eq $sceneCount

$sceneActivityPresent = $capturesPresent
if ($capturesPresent) {
    foreach ($capture in @($baseline + $adaptive)) {
        if (-not (Test-ExplicitBoolean $capture captured $true) -or
            -not (Test-ExplicitBoolean $capture history_complete $true) -or
            -not (Test-FiniteNumber $capture.active_resources 1 ([double]::MaxValue))) {
            $sceneActivityPresent = $false
            break
        }
    }
}

$matrixShapeValid = Test-DistanceMatrix $matrix $sceneCount

$sameThreshold = 0.22
if ($null -ne $scene -and (Test-FiniteNumber $scene.same_scene_threshold 0.000001 1.0)) {
    $sameThreshold = [double]$scene.same_scene_threshold
}

$matches = 0
$withinThreshold = 0
$positiveMargins = 0
$clusterMatches = 0
$sameDistances = @()
$identityMargins = @()

if ($matrixShapeValid -and $capturesPresent) {
    for ($j = 0; $j -lt $sceneCount; ++$j) {
        $best = [double]::PositiveInfinity
        $bestWrong = [double]::PositiveInfinity
        $bestIndex = -1
        $diagonal = [double](@($matrix[$j])[$j])
        $sameDistances += $diagonal

        if ($diagonal -le $sameThreshold) { ++$withinThreshold }

        for ($i = 0; $i -lt $sceneCount; ++$i) {
            $distance = [double](@($matrix[$i])[$j])
            if ($distance -lt $best) {
                $best = $distance
                $bestIndex = $i
            }
            if ($i -ne $j -and $distance -lt $bestWrong) {
                $bestWrong = $distance
            }
        }

        if ($bestIndex -eq $j) { ++$matches }

        $margin = 0.0
        if (-not [double]::IsInfinity($bestWrong)) {
            $margin = $bestWrong - $diagonal
        }
        $identityMargins += $margin
        if ($margin -gt 0.0) { ++$positiveMargins }

        if ([int64]$baseline[$j].cluster -gt 0 -and
            [int64]$baseline[$j].cluster -eq [int64]$adaptive[$j].cluster) {
            ++$clusterMatches
        }
    }
}

$sceneMatchAccuracy = if ($sceneCount) { [double]$matches / [double]$sceneCount } else { 0.0 }
$sameThresholdRatio = if ($sceneCount) { [double]$withinThreshold / [double]$sceneCount } else { 0.0 }
$positiveMarginRatio = if ($sceneCount) { [double]$positiveMargins / [double]$sceneCount } else { 0.0 }
$clusterRecurrence = if ($sceneCount) { [double]$clusterMatches / [double]$sceneCount } else { 0.0 }
$meanSameDistance = Mean-Value $sameDistances
$meanIdentityMargin = Mean-Value $identityMargins

$gates = [ordered]@{
    underlying_stage14_5_pass = ($stage14Exit -eq 0 -and (Test-ExplicitBoolean $raw valid $true))
    semantic_block_present = ($null -ne $sem)
    semantic_observer_only = (Test-ExplicitBoolean $sem observer_only $true)
    measured_semantic_population = ($null -ne $sem -and $sem.capture_phase -eq 'adaptive_end_before_recovery' -and (Test-FiniteNumber $sem.capture_frame 1 ([double]::MaxValue)))
    heuristic_confidence_declared = ($null -ne $sem -and $sem.confidence_basis -eq 'heuristic_score' -and (Test-ExplicitBoolean $sem accuracy_validated $false))
    semantic_scores_valid = ($null -ne $sem -and (Test-FiniteNumber $sem.coverage 0 1) -and (Test-FiniteNumber $sem.mean_confidence 0 1) -and (Test-FiniteNumber $sem.high_confidence_ratio 0 1))
    complex_resource_population = ($null -ne $sem -and [int64]$sem.alive_resources -ge 128)
    useful_semantic_coverage = ($null -ne $sem -and [double]$sem.coverage -ge 0.20)
    confidence_floor = ($null -ne $sem -and [double]$sem.mean_confidence -ge 0.65)
    high_confidence_population = ($null -ne $sem -and [double]$sem.high_confidence_ratio -ge 0.50)
    scene_block_present = ($null -ne $scene)
    scene_observer_only = (Test-ExplicitBoolean $scene observer_only $true)
    truth_labels_not_used_for_inference = (Test-ExplicitBoolean $scene truth_labels_used_for_inference $false)
    same_scene_threshold_valid = ($null -ne $scene -and (Test-FiniteNumber $scene.same_scene_threshold 0.000001 1.0))
    truth_set_valid = $truthLabelsUnique
    scene_captures_complete = $capturesPresent
    scene_activity_present = $sceneActivityPresent
    distance_matrix_valid = $matrixShapeValid
    scene_retrieval_accuracy = ($sceneMatchAccuracy -ge 0.80)
    same_scene_threshold_recall = ($sameThresholdRatio -ge 0.80)
    positive_identity_margin = ($positiveMarginRatio -ge 0.80)
    identity_margin_floor = ($meanIdentityMargin -ge 0.02)
    cluster_recurrence = ($clusterRecurrence -ge 0.80)
    nontrivial_clustering = ($null -ne $scene -and [int64]$scene.cluster_count -ge 2)
    controller_blind_to_truth = (Test-ExplicitBoolean $raw semantic_labels_used_by_controller $false)
    graph_clean = (Test-FiniteNumber $raw.graph.errors 0 0)
    backend_clean = (Test-FiniteNumber $raw.governor.quality_backend_failures 0 0)
}

$passed = $true
foreach ($value in $gates.Values) {
    $passed = $passed -and [bool]$value
}

$acceptance = [ordered]@{
    schema = 3
    stage = '15-scene-understanding'
    verdict = $(if ($passed) { 'PASS' } else { 'FAIL' })
    source_sha = $sourceSha
    wicked_sha = [string]$raw.wicked_upstream_sha
    gates = $gates
    validation_scope = [ordered]@{
        semantic_confidence_basis = 'heuristic_score'
        resource_semantic_accuracy_validated = $false
        scene_identity_evaluated = $true
        performance_comparison = 'adaptive_vs_observer_baseline'
        arc_off_comparison_available = $false
        product_performance_validated = $false
    }
    metrics = [ordered]@{
        alive_resources = $(if ($sem) { [int64]$sem.alive_resources } else { 0 })
        known_resources = $(if ($sem) { [int64]$sem.known_resources } else { 0 })
        semantic_coverage = $(if ($sem) { [double]$sem.coverage } else { 0.0 })
        mean_confidence = $(if ($sem) { [double]$sem.mean_confidence } else { 0.0 })
        high_confidence_ratio = $(if ($sem) { [double]$sem.high_confidence_ratio } else { 0.0 })
        class_counts = $(if ($sem) { @($sem.counts) } else { @() })
        scene_count = $sceneCount
        discovered_clusters = $(if ($scene) { [int64]$scene.cluster_count } else { 0 })
        same_scene_threshold = $sameThreshold
        scene_match_accuracy = $sceneMatchAccuracy
        same_scene_threshold_ratio = $sameThresholdRatio
        positive_identity_margin_ratio = $positiveMarginRatio
        mean_same_scene_distance = $meanSameDistance
        mean_identity_margin = $meanIdentityMargin
        cluster_recurrence_ratio = $clusterRecurrence
        identity_margins = $identityMargins
    }
    note = 'PASS applies only to the Stage 15 observer and scene-identity gates for this run. Semantic coverage is not resource-role accuracy; confidence is an uncalibrated heuristic score. This experiment does not establish total overhead or product speedup relative to ARC OFF. Truth labels are post-run evaluation data only.'
}
Json-Write $acceptance (Join-Path $run.FullName 'stage15-acceptance.json')

$summary = @"

## Mega C / Stage 15
- Verdict: **$($acceptance.verdict)**
- Observer-only inference: $($gates.scene_observer_only)
- Explicit truth isolation verified (inference): $($gates.truth_labels_not_used_for_inference)
- Explicit truth isolation verified (controller): $($gates.controller_blind_to_truth)
- Resource semantic coverage: $([math]::Round(100.0 * $acceptance.metrics.semantic_coverage, 2))%
- Resource mean heuristic score (uncalibrated): $([math]::Round($acceptance.metrics.mean_confidence, 3))
- Scene retrieval accuracy: $([math]::Round(100.0 * $sceneMatchAccuracy, 2))%
- Same-scene threshold recall: $([math]::Round(100.0 * $sameThresholdRatio, 2))%
- Positive identity margin: $([math]::Round(100.0 * $positiveMarginRatio, 2))%
- Mean identity margin: $([math]::Round($meanIdentityMargin, 4))
- Cluster recurrence: $([math]::Round(100.0 * $clusterRecurrence, 2))%
- Discovered clusters: $($acceptance.metrics.discovered_clusters)
"@
Add-Content -LiteralPath (Join-Path $run.FullName 'SUMMARY.md') -Value $summary -Encoding utf8

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $arc 'scripts\stage15-wicked-publish-existing.ps1') -RepoRoot $arc -RunStamp $run.Name
if ($LASTEXITCODE -ne 0) { throw "Publishing failed: $LASTEXITCODE" }

Write-Host ""
Write-Host "Stage 15: $($acceptance.verdict)"
Write-Host "Scene retrieval: $([math]::Round(100.0 * $sceneMatchAccuracy,2))%"
Write-Host "Same-scene threshold recall: $([math]::Round(100.0 * $sameThresholdRatio,2))%"
Write-Host "Positive identity margin: $([math]::Round(100.0 * $positiveMarginRatio,2))%"
Write-Host "Cluster recurrence: $([math]::Round(100.0 * $clusterRecurrence,2))%"
$stage15Url = Join-Path $localRoot 'last-stage15-result-url.txt'
if (Test-Path -LiteralPath $stage15Url) {
    Write-Host "Results URL: $((Get-Content -LiteralPath $stage15Url -Raw).Trim())"
}

if ($passed) { exit 0 }
exit 2
