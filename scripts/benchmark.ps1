param([int]$Iterations = 3000, [int]$Rounds = 3)
$ErrorActionPreference = 'Stop'
if ($Rounds -lt 1 -or $Rounds -gt 10) { throw 'Rounds must be 1..10' }
$measurements = @()
$modes = @('baseline', 'light', 'full')
for ($round = 0; $round -lt $Rounds; $round++) {
    for ($step = 0; $step -lt 3; $step++) {
        $mode = $modes[($step + $round) % 3]
        & ./build/Release/dx12-mixed-resources.exe $mode $Iterations
        if ($LASTEXITCODE -ne 0) { throw "Validation failed for $mode round $round" }
        $metrics = Get-Content -Raw "traces/dx12-mixed-resources-$mode-metrics.json" | ConvertFrom-Json
        $correctness = Get-Content -Raw "traces/dx12-mixed-resources-$mode.json" | ConvertFrom-Json
        $measurements += [pscustomobject]@{
            round = $round; mode = $mode; iterations = $Iterations; p50_ms = $correctness.iteration_ms_p50
            p95_ms = $correctness.iteration_ms_p95; p99_ms = $correctness.iteration_ms_p99
            cpu_ms = $metrics.cpu_ms; wall_ms = $metrics.wall_ms; private_bytes = $metrics.process_private_bytes
            producer_cpu_ms = $metrics.producer_thread_cpu_ms; collector_cpu_ms = $metrics.collector_cpu_ms
            writer_cpu_ms = $metrics.trace_writer_cpu_ms; offline_graph_cpu_ms = $metrics.offline_graph_cpu_ms
            trace_chunks = $metrics.trace_chunks; trace_checkpoints = $metrics.trace_checkpoints
            trace_bytes = $metrics.trace_bytes; trace_mb_s = $metrics.trace_bytes / 1000 / $metrics.wall_ms
            valid = $correctness.valid; dropped = $correctness.dropped; occluded_presents = $metrics.occluded_presents
        }
    }
}
$measurements | ConvertTo-Json -Depth 5 | Set-Content traces/benchmark-matrix.json -Encoding utf8
$measurements | Format-Table round,mode,p50_ms,p99_ms,cpu_ms,private_bytes,valid,dropped
