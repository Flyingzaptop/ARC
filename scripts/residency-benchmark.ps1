param([int]$Rounds = 6, [int]$Objects = 24, [int]$ObjectMiB = 2)
$ErrorActionPreference = 'Stop'
if ($Rounds -lt 3 -or $Rounds -gt 30) { throw 'Rounds must be 3..30' }
if ($Objects -lt 8 -or $Objects -gt 128) { throw 'Objects must be 8..128' }
if ($ObjectMiB -lt 1 -or $ObjectMiB -gt 64) { throw 'ObjectMiB must be 1..64' }
$orders = @(
    @('baseline', 'arc', 'auto'),
    @('auto', 'baseline', 'arc'),
    @('arc', 'auto', 'baseline')
)
$results = @()
$env:ARC_D3D12_DEBUG = '1'
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
$auto = @($results | Where-Object mode -eq 'auto')
if ($auto.Count -ne $Rounds) { throw 'Autonomous mode did not produce one result per round' }
$summary = [ordered]@{
    schema = 1
    rounds = $Rounds
    objects = $Objects
    object_mib = $ObjectMiB
    auto = [ordered]@{
        mean_late_residency = ($auto | Measure-Object late_residency -Average).Average
        mean_false_evictions = ($auto | Measure-Object false_evictions -Average).Average
        mean_managed_usage = ($auto | Measure-Object managed_usage_average -Average).Average
        mean_p50_ms = ($auto | Measure-Object p50_ms -Average).Average
        mean_p95_ms = ($auto | Measure-Object p95_ms -Average).Average
        mean_p99_ms = ($auto | Measure-Object p99_ms -Average).Average
    }
}
$summary | ConvertTo-Json -Depth 8 | Set-Content traces/residency-benchmark-summary.json -Encoding utf8
Write-Host "Saved traces/residency-benchmark.json and traces/residency-benchmark-summary.json"
