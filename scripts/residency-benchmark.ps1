param([int]$Rounds = 6, [int]$Objects = 24, [int]$ObjectMiB = 2)
$ErrorActionPreference = 'Stop'
if ($Rounds -lt 3 -or $Rounds -gt 30) { throw 'Rounds must be 3..30' }
if ($Objects -lt 8 -or $Objects -gt 128) { throw 'Objects must be 8..128' }
if ($ObjectMiB -lt 1 -or $ObjectMiB -gt 64) { throw 'ObjectMiB must be 1..64' }
# Performance measurements intentionally run without the D3D12 debug layer.
Remove-Item Env:ARC_D3D12_DEBUG -ErrorAction SilentlyContinue
$orders = @(
    @('baseline', 'arc', 'auto'),
    @('auto', 'baseline', 'arc'),
    @('arc', 'auto', 'baseline')
)
$results = @()
for ($round = 0; $round -lt $Rounds; $round++) {
    $order = $orders[$round % $orders.Count]
    for ($position = 0; $position -lt $order.Count; $position++) {
        $mode = $order[$position]
        & ./build/Release/dx12-residency-lab.exe $mode $Objects $ObjectMiB
        if ($LASTEXITCODE -ne 0) { throw "Residency lab failed: $mode round $round position $position" }
        $path = "traces/residency-lab-$mode.json"
        if (-not (Test-Path $path)) { throw "Missing residency report: $path" }
        $result = Get-Content -Raw $path | ConvertFrom-Json
        if (-not $result.valid) { throw "Residency report invalid: $mode round $round" }
        if (-not $result.contents_identical) { throw "GPU content mismatch: $mode round $round" }
        if ($result.trace_drops -ne 0 -or $result.graph_errors -ne 0) { throw "Trace/graph error: $mode round $round" }
        $result | Add-Member -NotePropertyName round -NotePropertyValue $round
        $result | Add-Member -NotePropertyName run_position -NotePropertyValue $position
        $result | Add-Member -NotePropertyName run_order -NotePropertyValue ($order -join ',')
        $results += $result
    }
}
New-Item -ItemType Directory -Force traces | Out-Null
$results | ConvertTo-Json -Depth 8 | Set-Content traces/residency-benchmark.json -Encoding utf8
$results | Format-Table round,run_position,mode,valid,managed_usage_average,managed_usage_peak,bytes_evicted,reloads,false_evictions,late_residency,speculative_promotions,p50_ms,p95_ms,p99_ms
function Mean-Property($items, [string]$property) {
    return ($items | Measure-Object -Property $property -Average).Average
}
function Mode-Summary([string]$mode) {
    $items = @($results | Where-Object mode -eq $mode)
    if ($items.Count -ne $Rounds) { throw "$mode did not produce one result per round" }
    return [ordered]@{
        runs = $items.Count
        mean_p50_ms = Mean-Property $items 'p50_ms'
        mean_p95_ms = Mean-Property $items 'p95_ms'
        mean_p99_ms = Mean-Property $items 'p99_ms'
        mean_bytes_evicted = Mean-Property $items 'bytes_evicted'
        mean_reloads = Mean-Property $items 'reloads'
        mean_false_evictions = Mean-Property $items 'false_evictions'
        mean_late_residency = Mean-Property $items 'late_residency'
        mean_speculative_promotions = Mean-Property $items 'speculative_promotions'
        mean_managed_usage = Mean-Property $items 'managed_usage_average'
        mean_managed_peak = Mean-Property $items 'managed_usage_peak'
    }
}
$baseline = Mode-Summary 'baseline'
$oracle = Mode-Summary 'arc'
$auto = Mode-Summary 'auto'
$autoItems = @($results | Where-Object mode -eq 'auto')
$managedFractions = @($autoItems | ForEach-Object { if ($_.managed_budget -gt 0) { [double]$_.managed_usage_average / [double]$_.managed_budget } })
$meanManagedFraction = if ($managedFractions.Count) { ($managedFractions | Measure-Object -Average).Average } else { $null }
$summary = [ordered]@{
    schema = 2
    rounds = $Rounds
    objects = $Objects
    object_mib = $ObjectMiB
    debug_layer = $false
    baseline = $baseline
    oracle = $oracle
    auto = $auto
    comparison = [ordered]@{
        auto_minus_baseline_p50_ms = [double]$auto.mean_p50_ms - [double]$baseline.mean_p50_ms
        auto_minus_baseline_p95_ms = [double]$auto.mean_p95_ms - [double]$baseline.mean_p95_ms
        auto_minus_baseline_p99_ms = [double]$auto.mean_p99_ms - [double]$baseline.mean_p99_ms
        auto_minus_oracle_p99_ms = [double]$auto.mean_p99_ms - [double]$oracle.mean_p99_ms
        auto_mean_managed_budget_fraction = $meanManagedFraction
    }
}
$summary | ConvertTo-Json -Depth 10 | Set-Content traces/residency-benchmark-summary.json -Encoding utf8
Write-Host ''
Write-Host '=== Mean summary ==='
[pscustomobject]@{ mode='baseline'; p50=$baseline.mean_p50_ms; p95=$baseline.mean_p95_ms; p99=$baseline.mean_p99_ms; late=$baseline.mean_late_residency; managed=$baseline.mean_managed_usage } | Format-Table -AutoSize
[pscustomobject]@{ mode='oracle'; p50=$oracle.mean_p50_ms; p95=$oracle.mean_p95_ms; p99=$oracle.mean_p99_ms; late=$oracle.mean_late_residency; managed=$oracle.mean_managed_usage } | Format-Table -AutoSize
[pscustomobject]@{ mode='auto'; p50=$auto.mean_p50_ms; p95=$auto.mean_p95_ms; p99=$auto.mean_p99_ms; late=$auto.mean_late_residency; managed=$auto.mean_managed_usage } | Format-Table -AutoSize
Write-Host "auto-baseline P99 delta: $($summary.comparison.auto_minus_baseline_p99_ms) ms"
Write-Host "auto mean managed budget fraction: $meanManagedFraction"
Write-Host "Saved traces/residency-benchmark.json and traces/residency-benchmark-summary.json"
