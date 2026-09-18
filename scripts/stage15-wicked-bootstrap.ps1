param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/mega-c-stage15-scene-understanding',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-MegaC-Stage15",
    [string]$ExpectedSha = '',
    [ValidateRange(10,120)][int]$Seconds = 30
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $Path -Encoding utf8
}

$arc = Join-Path $WorkRoot 'arc'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null

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
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stage14 -RepoUrl $RepoUrl -Branch $Branch -WorkRoot $WorkRoot -ExpectedSha $sourceSha -Seconds $Seconds -NoPublish
$stage14Exit = $LASTEXITCODE

$localRoot = Join-Path $arc 'results\stage14_5-wicked-local'
$run = Get-ChildItem -LiteralPath $localRoot -Directory | Sort-Object Name -Descending | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'wicked-engine.json') } | Select-Object -First 1
if (-not $run) { throw 'Stage 14.5 did not produce a local Wicked result.' }

$rawPath = Join-Path $run.FullName 'wicked-engine.json'
$raw = Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
$sem = $raw.stage15_semantics

$gates = [ordered]@{
    underlying_stage14_5_pass = ($stage14Exit -eq 0 -and [bool]$raw.valid)
    semantic_block_present = ($null -ne $sem)
    observer_only = ($null -ne $sem -and [bool]$sem.observer_only)
    complex_resource_population = ($null -ne $sem -and [int64]$sem.alive_resources -ge 128)
    useful_semantic_coverage = ($null -ne $sem -and [double]$sem.coverage -ge 0.20)
    confidence_floor = ($null -ne $sem -and [double]$sem.mean_confidence -ge 0.65)
    high_confidence_population = ($null -ne $sem -and [double]$sem.high_confidence_ratio -ge 0.50)
    controller_blind_to_truth = (-not [bool]$raw.semantic_labels_used_by_controller)
    graph_clean = ([int64]$raw.graph.errors -eq 0)
    backend_clean = ([int64]$raw.governor.quality_backend_failures -eq 0)
}

$passed = $true
foreach ($v in $gates.Values) { $passed = $passed -and [bool]$v }

$acceptance = [ordered]@{
    schema = 1
    stage = '15-phase-a'
    verdict = $(if ($passed) { 'PASS' } else { 'FAIL' })
    source_sha = $sourceSha
    wicked_sha = [string]$raw.wicked_upstream_sha
    gates = $gates
    metrics = [ordered]@{
        alive_resources = $(if ($sem) { [int64]$sem.alive_resources } else { 0 })
        known_resources = $(if ($sem) { [int64]$sem.known_resources } else { 0 })
        semantic_coverage = $(if ($sem) { [double]$sem.coverage } else { 0.0 })
        mean_confidence = $(if ($sem) { [double]$sem.mean_confidence } else { 0.0 })
        high_confidence_ratio = $(if ($sem) { [double]$sem.high_confidence_ratio } else { 0.0 })
        class_counts = $(if ($sem) { @($sem.counts) } else { @() })
    }
    note = 'Phase A validates observer-only behavioral semantics. It does not close Stage 15; external truth-label accuracy is still required.'
}
Json-Write $acceptance (Join-Path $run.FullName 'stage15-acceptance.json')

$summary = @"

## Mega C / Stage 15 Phase A
- Verdict: **$($acceptance.verdict)**
- Observer only: $($gates.observer_only)
- Alive resources: $($acceptance.metrics.alive_resources)
- Known resources: $($acceptance.metrics.known_resources)
- Coverage: $([math]::Round(100.0 * $acceptance.metrics.semantic_coverage, 2))%
- Mean confidence: $([math]::Round($acceptance.metrics.mean_confidence, 3))
- High-confidence fraction: $([math]::Round(100.0 * $acceptance.metrics.high_confidence_ratio, 2))%
- Truth labels supplied to controller: **NO**
- Stage 15 closed: **NO - truth-label accuracy remains**
"@
Add-Content -LiteralPath (Join-Path $run.FullName 'SUMMARY.md') -Value $summary -Encoding utf8

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $arc 'scripts\stage14_5-wicked-publish-existing.ps1') -RepoRoot $arc -RunStamp $run.Name
if ($LASTEXITCODE -ne 0) { throw "Publishing failed: $LASTEXITCODE" }

Write-Host ""
Write-Host "Stage 15 Phase A: $($acceptance.verdict)"
Write-Host "Coverage: $([math]::Round(100.0 * $acceptance.metrics.semantic_coverage,2))%"
Write-Host "Mean confidence: $([math]::Round($acceptance.metrics.mean_confidence,3))"
Write-Host "High-confidence: $([math]::Round(100.0 * $acceptance.metrics.high_confidence_ratio,2))%"
Write-Host "Stage 15 remains open until truth-label accuracy is validated."

if ($passed) { exit 0 }
exit 2