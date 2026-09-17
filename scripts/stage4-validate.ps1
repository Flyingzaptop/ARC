param(
    [ValidateRange(3, 30)][int]$Stage2Rounds = 6,
    [ValidateRange(3000, 100000)][int]$ObserverIterations = 15000,
    [ValidateRange(3, 15)][int]$ObserverPairs = 5,
    [switch]$SkipDebugBuild,
    [switch]$Quick,
    [switch]$PublishResults
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force traces | Out-Null
    if ($Quick) {
        $Stage2Rounds = 3
        $ObserverIterations = 3000
        $ObserverPairs = 3
    }

    $commit = (git rev-parse HEAD).Trim()
    Write-Host '=== ARC Stage 4 live-runtime acceptance ==='
    Write-Host "commit: $commit"

    Write-Host ''
    Write-Host '[1/3] Stage 2 regression acceptance + all GPU labs'
    & "$PSScriptRoot/stage2-final-validate.ps1" -Rounds $Stage2Rounds -ObserverIterations 3000 -SkipDebugBuild:$SkipDebugBuild -Quick:$Quick
    if ($LASTEXITCODE -ne 0) { throw "Stage 2 regression acceptance failed: $LASTEXITCODE" }

    if (-not (Test-Path -LiteralPath 'traces/live-runtime-lab.json')) {
        throw 'Live runtime GPU report missing after Stage 2 regression run.'
    }

    Write-Host ''
    Write-Host '[2/3] Paired observer CPU benchmark v2'
    & "$PSScriptRoot/benchmark-v2.ps1" -Iterations $ObserverIterations -Pairs $ObserverPairs
    if ($LASTEXITCODE -ne 0) { throw "Observer benchmark v2 failed: $LASTEXITCODE" }

    Write-Host ''
    Write-Host '[3/3] Integrated Stage 4 verdict'
    $stage2 = Get-Content -Raw -LiteralPath 'traces/stage2-final-acceptance.json' | ConvertFrom-Json
    $live = Get-Content -Raw -LiteralPath 'traces/live-runtime-lab.json' | ConvertFrom-Json
    $observer = Get-Content -Raw -LiteralPath 'traces/observer-benchmark-v2.json' | ConvertFrom-Json
    $frontier = Get-Content -Raw -LiteralPath 'traces/residency-frontier-summary.json' | ConvertFrom-Json

    $liveCorrect = ([bool]$live.valid -and [bool]$live.contents_verified -and
        [bool]$live.unknown_resource_untouched -and [bool]$live.transition_prefetch_verified -and
        [int64]$live.eviction_actions -gt 0 -and [int64]$live.debug_error_count -eq 0)
    $stage2Correct = [bool]$stage2.gates.hard_correctness
    $debugAvailable = [bool]$stage2.gates.debug_layer_available
    $debugClean = [bool]$stage2.gates.debug_layer_clean
    $frontierPointFound = ($null -ne $frontier.recommended_lab_point)
    $cpuMet = [bool]$observer.light.cpu_target_met
    $p99Met = [bool]$observer.light.p99_target_met

    $verdict = 'ACCEPTED'
    if (-not $stage2Correct -or -not $liveCorrect) {
        $verdict = 'NOT_ACCEPTED'
    } elseif (-not $debugAvailable) {
        $verdict = 'ACCEPTED_WITH_EXTERNAL_VALIDATION_PENDING'
    } elseif (-not $debugClean -or -not $frontierPointFound -or -not $cpuMet -or -not $p99Met) {
        $verdict = 'ACCEPTED_WITH_PERFORMANCE_WARNINGS'
    }

    $trackedStatus = (git status --porcelain --untracked-files=no | Out-String).Trim()
    $acceptance = [ordered]@{
        schema = 1
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        commit = $commit
        working_tree_dirty = -not [string]::IsNullOrWhiteSpace($trackedStatus)
        verdict = $verdict
        gates = [ordered]@{
            stage2_hard_correctness = $stage2Correct
            live_runtime_correctness = $liveCorrect
            live_runtime_unknown_resource_untouched = [bool]$live.unknown_resource_untouched
            live_runtime_transition_prefetch = [bool]$live.transition_prefetch_verified
            live_runtime_dxgi_relief_bytes = [int64]$live.dxgi_observed_relief_bytes
            debug_layer_available = $debugAvailable
            debug_layer_clean = $debugClean
            frontier_operating_point_found = $frontierPointFound
            observer_v2_cpu_target_met = $cpuMet
            observer_v2_p99_target_met = $p99Met
            observer_v2_light_cpu_median_percent = [double]$observer.light.attributable_cpu_percent_median
            observer_v2_light_cpu_p90_percent = [double]$observer.light.attributable_cpu_percent_p90
            observer_v2_light_p99_delta_median_ms = [double]$observer.light.p99_delta_ms_median
            observer_v2_light_p99_delta_p90_ms = [double]$observer.light.p99_delta_ms_p90
        }
        live_runtime = $live
        observer_v2 = $observer
        stage2_acceptance_path = 'traces/stage2-final-acceptance.json'
        hardware_profile_path = 'traces/hardware-profile.json'
    }
    $acceptance | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath 'traces/stage4-acceptance.json' -Encoding utf8

    $report = @"
# ARC Stage 4 Acceptance

- Verdict: **$verdict**
- Commit: $commit

## Core gates

- Stage 2 regression: $stage2Correct
- Live runtime D3D12 path: $liveCorrect
- Unknown resource guard: $([bool]$live.unknown_resource_untouched)
- Transition prefetch: $([bool]$live.transition_prefetch_verified)
- DXGI relief: $([int64]$live.dxgi_observed_relief_bytes) bytes
- Debug layer clean: $debugClean

## Observer benchmark v2

- Light CPU median: $([Math]::Round([double]$observer.light.attributable_cpu_percent_median, 3)) %
- Light CPU p90: $([Math]::Round([double]$observer.light.attributable_cpu_percent_p90, 3)) %
- P99 median delta: $([Math]::Round([double]$observer.light.p99_delta_ms_median, 4)) ms
- P99 p90 delta: $([Math]::Round([double]$observer.light.p99_delta_ms_p90, 4)) ms
- CPU <= 2%: $cpuMet
- P99 <= 0.15 ms: $p99Met
"@
    $report | Set-Content -LiteralPath 'traces/STAGE4_ACCEPTANCE.md' -Encoding utf8

    Write-Host ''
    Write-Host '=== STAGE 4 VERDICT ==='
    Write-Host $verdict
    Write-Host "Live runtime: valid=$($live.valid) prefetch=$($live.transition_prefetch_actions) dxgiRelief=$($live.dxgi_observed_relief_bytes)"
    Write-Host "Observer v2 Light CPU median=$([Math]::Round([double]$observer.light.attributable_cpu_percent_median, 3))% p90=$([Math]::Round([double]$observer.light.attributable_cpu_percent_p90, 3))%"

    if ($PublishResults) {
        & "$PSScriptRoot/publish-stage4-results.ps1"
        if ($LASTEXITCODE -ne 0) { throw "Stage 4 results publication failed: $LASTEXITCODE" }
        if (Test-Path -LiteralPath 'traces/stage4-results-url.txt') {
            Write-Host "Results URL: $((Get-Content -Raw -LiteralPath 'traces/stage4-results-url.txt').Trim())"
        }
    }

    if ($verdict -eq 'NOT_ACCEPTED') { exit 2 }
} finally {
    Pop-Location
}
