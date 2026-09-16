param(
    [ValidateRange(1, 10)][int]$Rounds = 2,
    [ValidateRange(8, 128)][int]$Objects = 24,
    [ValidateRange(1, 64)][int]$ObjectMiB = 2,
    [ValidateRange(100, 5000)][int]$MeasuredEpochs = 1200,
    [ValidateRange(50, 2000)][int]$WarmupEpochs = 300
)

$ErrorActionPreference = 'Stop'
$exe = './build/Release/dx12-residency-lab.exe'
if (-not (Test-Path $exe)) { throw 'Release residency lab is missing. Build Release first.' }

$patterns = @('mixed', 'cycle', 'phase', 'stream')
$managedPercents = @(45, 55, 65, 75, 85)
$results = @()
$env:ARC_D3D12_DEBUG = '1'
New-Item -ItemType Directory -Force traces | Out-Null

function Invoke-Lab([string]$mode, [string]$pattern, [int]$managedPercent, [int]$round, [int]$position) {
    & $exe $mode $Objects $ObjectMiB $managedPercent $pattern $WarmupEpochs $MeasuredEpochs
    if ($LASTEXITCODE -ne 0) { throw "Residency lab failed: mode=$mode pattern=$pattern managed=$managedPercent round=$round" }
    $path = "traces/residency-lab-$mode.json"
    if (-not (Test-Path $path)) { throw "Missing residency report: $path" }
    $result = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if (-not $result.valid -or -not $result.contents_identical) { throw "Invalid residency result: mode=$mode pattern=$pattern" }
    if ($result.trace_drops -ne 0 -or $result.graph_errors -ne 0) { throw "Trace/graph error: mode=$mode pattern=$pattern" }
    $result | Add-Member -NotePropertyName round -NotePropertyValue $round
    $result | Add-Member -NotePropertyName run_position -NotePropertyValue $position
    return $result
}

for ($round = 0; $round -lt $Rounds; $round++) {
    $jobs = @()
    foreach ($pattern in $patterns) {
        $jobs += [pscustomobject]@{ mode='baseline'; pattern=$pattern; managed=100 }
        $jobs += [pscustomobject]@{ mode='arc'; pattern=$pattern; managed=100 }
        foreach ($percent in $managedPercents) {
            $jobs += [pscustomobject]@{ mode='auto'; pattern=$pattern; managed=$percent }
        }
    }
    # Deterministic rotation/reversal avoids always favoring the same mode with thermals/caches.
    if (($round % 2) -eq 1) { [array]::Reverse($jobs) }
    $offset = ($round * 7) % $jobs.Count
    if ($offset -gt 0) { $jobs = @($jobs[$offset..($jobs.Count-1)] + $jobs[0..($offset-1)]) }
    for ($position = 0; $position -lt $jobs.Count; $position++) {
        $job = $jobs[$position]
        $results += Invoke-Lab $job.mode $job.pattern $job.managed $round $position
    }
}

$rawPath = 'traces/residency-frontier.json'
$summaryPath = 'traces/residency-frontier-summary.json'
$csvPath = 'traces/residency-frontier.csv'
$results | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $rawPath -Encoding utf8
$results | Select-Object round,run_position,mode,pattern,managed_percent,p50_ms,p95_ms,p99_ms,managed_usage_average,managed_usage_peak,late_residency,compulsory_misses,predictable_misses,bytes_evicted,reloads | Export-Csv -NoTypeInformation -LiteralPath $csvPath

function Mean($items, [string]$property) {
    $values = @($items | ForEach-Object { [double]($_.$property) })
    if (-not $values.Count) { return 0.0 }
    return ($values | Measure-Object -Average).Average
}

$patternSummaries = @()
foreach ($pattern in $patterns) {
    $baselineItems = @($results | Where-Object { $_.pattern -eq $pattern -and $_.mode -eq 'baseline' })
    $oracleItems = @($results | Where-Object { $_.pattern -eq $pattern -and $_.mode -eq 'arc' })
    $baselineP95 = Mean $baselineItems 'p95_ms'
    $baselineP99 = Mean $baselineItems 'p99_ms'
    $oracleP99 = Mean $oracleItems 'p99_ms'
    foreach ($percent in $managedPercents) {
        $items = @($results | Where-Object { $_.pattern -eq $pattern -and $_.mode -eq 'auto' -and [int]$_.managed_percent -eq $percent })
        if ($items.Count -ne $Rounds) { throw "Missing auto frontier samples for $pattern/$percent" }
        $fullWorkingSet = [double]$Objects * [double]$ObjectMiB * 1024.0 * 1024.0
        $meanManaged = Mean $items 'managed_usage_average'
        $late = Mean $items 'late_residency'
        $predictable = Mean $items 'predictable_misses'
        $compulsory = Mean $items 'compulsory_misses'
        $patternSummaries += [pscustomobject][ordered]@{
            pattern = $pattern
            managed_percent = $percent
            runs = $items.Count
            mean_p50_ms = Mean $items 'p50_ms'
            mean_p95_ms = Mean $items 'p95_ms'
            mean_p99_ms = Mean $items 'p99_ms'
            baseline_p95_ms = $baselineP95
            baseline_p99_ms = $baselineP99
            oracle_p99_ms = $oracleP99
            p95_delta_ms = (Mean $items 'p95_ms') - $baselineP95
            p99_delta_ms = (Mean $items 'p99_ms') - $baselineP99
            mean_managed_bytes = $meanManaged
            full_working_set_fraction = if ($fullWorkingSet -gt 0) { $meanManaged / $fullWorkingSet } else { 0 }
            mean_late_residency = $late
            mean_predictable_misses = $predictable
            mean_compulsory_misses = $compulsory
            predictable_misses_per_1000 = if ($MeasuredEpochs -gt 0) { 1000.0 * $predictable / $MeasuredEpochs } else { 0 }
            compulsory_misses_per_1000 = if ($MeasuredEpochs -gt 0) { 1000.0 * $compulsory / $MeasuredEpochs } else { 0 }
            mean_bytes_evicted = Mean $items 'bytes_evicted'
            mean_reloads = Mean $items 'reloads'
        }
    }
}

$aggregate = @()
foreach ($percent in $managedPercents) {
    $items = @($patternSummaries | Where-Object { $_.managed_percent -eq $percent })
    $aggregate += [pscustomobject][ordered]@{
        managed_percent = $percent
        mean_full_working_set_fraction = Mean $items 'full_working_set_fraction'
        mean_p95_delta_ms = Mean $items 'p95_delta_ms'
        mean_p99_delta_ms = Mean $items 'p99_delta_ms'
        worst_p99_delta_ms = ($items | Measure-Object -Property p99_delta_ms -Maximum).Maximum
        total_predictable_misses_per_1000 = ($items | Measure-Object -Property predictable_misses_per_1000 -Sum).Sum
        total_compulsory_misses_per_1000 = ($items | Measure-Object -Property compulsory_misses_per_1000 -Sum).Sum
    }
}

# A conservative lab-only operating-point suggestion. Production policy remains dynamic.
$eligible = @($aggregate | Where-Object { $_.worst_p99_delta_ms -le 0.15 -and $_.total_predictable_misses_per_1000 -le 1.0 } | Sort-Object mean_full_working_set_fraction)
$recommended = if ($eligible.Count) { $eligible[0] } else { $null }

$summary = [ordered]@{
    schema = 1
    rounds = $Rounds
    objects = $Objects
    object_mib = $ObjectMiB
    warmup_epochs = $WarmupEpochs
    measured_epochs = $MeasuredEpochs
    patterns = $patterns
    managed_percents = $managedPercents
    acceptance_guard = [ordered]@{ worst_p99_delta_ms_max = 0.15; total_predictable_misses_per_1000_max = 1.0 }
    pattern_points = $patternSummaries
    aggregate_points = $aggregate
    recommended_lab_point = $recommended
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host ''
Write-Host '=== Residency memory/latency frontier ==='
$aggregate | Format-Table managed_percent,mean_full_working_set_fraction,mean_p95_delta_ms,mean_p99_delta_ms,worst_p99_delta_ms,total_predictable_misses_per_1000,total_compulsory_misses_per_1000 -AutoSize
if ($recommended) {
    Write-Host ("Lab guard recommendation: managed={0}% resident_fraction={1:N3} worst_p99_delta={2:N3} ms" -f $recommended.managed_percent,$recommended.mean_full_working_set_fraction,$recommended.worst_p99_delta_ms)
} else {
    Write-Host 'No frontier point satisfies the conservative latency/predictable-miss guard yet.' -ForegroundColor Yellow
}
Write-Host "Saved $rawPath, $summaryPath and $csvPath"
