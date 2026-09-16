param([int]$Iterations = 3000, [int]$Rounds = 3)
$ErrorActionPreference = 'Stop'
if ($Rounds -lt 1 -or $Rounds -gt 10) { throw 'Rounds must be 1..10' }
if ($Iterations -lt 100 -or $Iterations -gt 1000000) { throw 'Iterations must be 100..1000000' }

$measurements = @()
$modes = @('baseline', 'light', 'full')
for ($round = 0; $round -lt $Rounds; $round++) {
    for ($step = 0; $step -lt 3; $step++) {
        $mode = $modes[($step + $round) % 3]
        & ./build/Release/dx12-mixed-resources.exe $mode $Iterations
        if ($LASTEXITCODE -ne 0) { throw "Validation failed for $mode round $round" }
        $metrics = Get-Content -Raw "traces/dx12-mixed-resources-$mode-metrics.json" | ConvertFrom-Json
        $correctness = Get-Content -Raw "traces/dx12-mixed-resources-$mode.json" | ConvertFrom-Json
        if (-not $correctness.valid -or $correctness.dropped -ne 0) { throw "Observer correctness failed for $mode round $round" }
        $measurements += [pscustomobject]@{
            round = $round
            mode = $mode
            iterations = $Iterations
            p50_ms = [double]$correctness.iteration_ms_p50
            p95_ms = [double]$correctness.iteration_ms_p95
            p99_ms = [double]$correctness.iteration_ms_p99
            cpu_ms = [double]$metrics.cpu_ms
            wall_ms = [double]$metrics.wall_ms
            private_bytes = [double]$metrics.process_private_bytes
            producer_cpu_ms = [double]$metrics.producer_thread_cpu_ms
            collector_cpu_ms = [double]$metrics.collector_cpu_ms
            writer_cpu_ms = [double]$metrics.trace_writer_cpu_ms
            offline_graph_cpu_ms = [double]$metrics.offline_graph_cpu_ms
            trace_chunks = [double]$metrics.trace_chunks
            trace_checkpoints = [double]$metrics.trace_checkpoints
            trace_bytes = [double]$metrics.trace_bytes
            trace_mb_s = if ($metrics.wall_ms -gt 0) { [double]$metrics.trace_bytes / 1000.0 / [double]$metrics.wall_ms } else { 0.0 }
            valid = [bool]$correctness.valid
            dropped = [double]$correctness.dropped
            occluded_presents = [double]$metrics.occluded_presents
        }
    }
}

New-Item -ItemType Directory -Force traces | Out-Null
$measurements | ConvertTo-Json -Depth 5 | Set-Content traces/benchmark-matrix.json -Encoding utf8
$measurements | Format-Table round,mode,p50_ms,p95_ms,p99_ms,cpu_ms,private_bytes,producer_cpu_ms,collector_cpu_ms,writer_cpu_ms,valid,dropped

function Mean($items, [string]$property) {
    $values = @($items | ForEach-Object { [double]($_.$property) })
    if (-not $values.Count) { return 0.0 }
    return ($values | Measure-Object -Average).Average
}
function Summarize([string]$mode) {
    $items = @($measurements | Where-Object mode -eq $mode)
    return [ordered]@{
        runs = $items.Count
        mean_p50_ms = Mean $items 'p50_ms'
        mean_p95_ms = Mean $items 'p95_ms'
        mean_p99_ms = Mean $items 'p99_ms'
        mean_cpu_ms = Mean $items 'cpu_ms'
        mean_wall_ms = Mean $items 'wall_ms'
        mean_private_bytes = Mean $items 'private_bytes'
        mean_producer_cpu_ms = Mean $items 'producer_cpu_ms'
        mean_collector_cpu_ms = Mean $items 'collector_cpu_ms'
        mean_writer_cpu_ms = Mean $items 'writer_cpu_ms'
        mean_offline_graph_cpu_ms = Mean $items 'offline_graph_cpu_ms'
        mean_trace_mb_s = Mean $items 'trace_mb_s'
    }
}

$baseline = Summarize 'baseline'
$light = Summarize 'light'
$full = Summarize 'full'
function PercentDelta([double]$value, [double]$reference) {
    if ($reference -eq 0) { return $null }
    return 100.0 * ($value - $reference) / $reference
}
$summary = [ordered]@{
    schema = 1
    rounds = $Rounds
    iterations = $Iterations
    baseline = $baseline
    light = $light
    full = $full
    light_delta = [ordered]@{
        p50_ms = [double]$light.mean_p50_ms - [double]$baseline.mean_p50_ms
        p95_ms = [double]$light.mean_p95_ms - [double]$baseline.mean_p95_ms
        p99_ms = [double]$light.mean_p99_ms - [double]$baseline.mean_p99_ms
        cpu_percent = PercentDelta $light.mean_cpu_ms $baseline.mean_cpu_ms
        private_bytes = [double]$light.mean_private_bytes - [double]$baseline.mean_private_bytes
        background_cpu_ms = [double]$light.mean_collector_cpu_ms + [double]$light.mean_writer_cpu_ms + [double]$light.mean_offline_graph_cpu_ms
    }
    full_delta = [ordered]@{
        p50_ms = [double]$full.mean_p50_ms - [double]$baseline.mean_p50_ms
        p95_ms = [double]$full.mean_p95_ms - [double]$baseline.mean_p95_ms
        p99_ms = [double]$full.mean_p99_ms - [double]$baseline.mean_p99_ms
        cpu_percent = PercentDelta $full.mean_cpu_ms $baseline.mean_cpu_ms
        private_bytes = [double]$full.mean_private_bytes - [double]$baseline.mean_private_bytes
        background_cpu_ms = [double]$full.mean_collector_cpu_ms + [double]$full.mean_writer_cpu_ms + [double]$full.mean_offline_graph_cpu_ms
    }
}
$summary | ConvertTo-Json -Depth 10 | Set-Content traces/benchmark-summary.json -Encoding utf8

Write-Host ''
Write-Host '=== Observer overhead summary ==='
[pscustomobject]@{mode='Light';p50_delta_ms=$summary.light_delta.p50_ms;p99_delta_ms=$summary.light_delta.p99_ms;cpu_delta_percent=$summary.light_delta.cpu_percent;background_cpu_ms=$summary.light_delta.background_cpu_ms;memory_delta_bytes=$summary.light_delta.private_bytes} | Format-Table -AutoSize
[pscustomobject]@{mode='Full';p50_delta_ms=$summary.full_delta.p50_ms;p99_delta_ms=$summary.full_delta.p99_ms;cpu_delta_percent=$summary.full_delta.cpu_percent;background_cpu_ms=$summary.full_delta.background_cpu_ms;memory_delta_bytes=$summary.full_delta.private_bytes} | Format-Table -AutoSize
Write-Host 'Saved traces/benchmark-matrix.json and traces/benchmark-summary.json'
