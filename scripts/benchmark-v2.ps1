param(
    [int]$Iterations = 15000,
    [int]$Pairs = 5,
    [UInt64]$AffinityMask = 0
)
$ErrorActionPreference = 'Stop'
if ($Pairs -lt 3 -or $Pairs -gt 15) { throw 'Pairs must be 3..15' }
if ($Iterations -lt 3000 -or $Iterations -gt 100000) { throw 'Iterations must be 3000..100000' }

$exe = Join-Path $PWD 'build/Release/dx12-mixed-resources.exe'
if (-not (Test-Path $exe)) { throw "Missing $exe" }
New-Item -ItemType Directory -Force traces | Out-Null

function Invoke-Mode([string]$Mode, [int]$Count) {
    $args = @($Mode, [string]$Count)
    $p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $PWD -PassThru -NoNewWindow
    if ($AffinityMask -ne 0) {
        try { $p.ProcessorAffinity = [IntPtr]([Int64]$AffinityMask) } catch { Write-Warning "Could not set affinity: $($_.Exception.Message)" }
    }
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw "Validation failed for $Mode with exit $($p.ExitCode)" }
    $metrics = Get-Content -Raw "traces/dx12-mixed-resources-$Mode-metrics.json" | ConvertFrom-Json
    $correctness = Get-Content -Raw "traces/dx12-mixed-resources-$Mode.json" | ConvertFrom-Json
    if (-not $correctness.valid -or [double]$correctness.dropped -ne 0) { throw "Observer correctness failed for $Mode" }
    [pscustomobject]@{
        mode = $Mode
        p50_ms = [double]$correctness.iteration_ms_p50
        p95_ms = [double]$correctness.iteration_ms_p95
        p99_ms = [double]$correctness.iteration_ms_p99
        process_cpu_ms = [double]$metrics.cpu_ms
        wall_ms = [double]$metrics.wall_ms
        producer_cpu_ms = [double]$metrics.producer_thread_cpu_ms
        background_cpu_ms = [double]$metrics.collector_cpu_ms + [double]$metrics.trace_writer_cpu_ms + [double]$metrics.offline_graph_cpu_ms
        private_bytes = [double]$metrics.process_private_bytes
        dropped = [double]$correctness.dropped
    }
}

function Median([double[]]$Values) {
    if (-not $Values.Count) { return 0.0 }
    $v = @($Values | Sort-Object)
    $n = $v.Count
    if (($n % 2) -eq 1) { return [double]$v[[int]($n / 2)] }
    return ([double]$v[$n/2 - 1] + [double]$v[$n/2]) / 2.0
}
function Percentile([double[]]$Values, [double]$Q) {
    if (-not $Values.Count) { return 0.0 }
    $v = @($Values | Sort-Object)
    if ($v.Count -eq 1) { return [double]$v[0] }
    $x = ($v.Count - 1) * $Q
    $lo = [int][math]::Floor($x); $hi = [int][math]::Ceiling($x)
    if ($lo -eq $hi) { return [double]$v[$lo] }
    return [double]$v[$lo] + ($x - $lo) * ([double]$v[$hi] - [double]$v[$lo])
}
function PairDelta($Base, $Mode) {
    $producerDelta = [double]$Mode.producer_cpu_ms - [double]$Base.producer_cpu_ms
    $attributable = $producerDelta + [double]$Mode.background_cpu_ms
    $denominator = [math]::Max(0.001, [double]$Base.producer_cpu_ms)
    [pscustomobject]@{
        baseline_producer_cpu_ms = [double]$Base.producer_cpu_ms
        mode_producer_cpu_ms = [double]$Mode.producer_cpu_ms
        mode_background_cpu_ms = [double]$Mode.background_cpu_ms
        producer_delta_ms = $producerDelta
        attributable_cpu_ms = $attributable
        attributable_cpu_percent = 100.0 * $attributable / $denominator
        process_cpu_percent = if ([double]$Base.process_cpu_ms -gt 0) { 100.0 * ([double]$Mode.process_cpu_ms - [double]$Base.process_cpu_ms) / [double]$Base.process_cpu_ms } else { $null }
        p99_delta_ms = [double]$Mode.p99_ms - [double]$Base.p99_ms
        p95_delta_ms = [double]$Mode.p95_ms - [double]$Base.p95_ms
        wall_ratio = [double]$Mode.wall_ms / [math]::Max(0.001, [double]$Base.wall_ms)
    }
}
function SummarizePairs($Items) {
    $cpu = [double[]]@($Items | ForEach-Object { [double]$_.attributable_cpu_percent })
    $p99 = [double[]]@($Items | ForEach-Object { [double]$_.p99_delta_ms })
    $proc = [double[]]@($Items | Where-Object { $null -ne $_.process_cpu_percent } | ForEach-Object { [double]$_.process_cpu_percent })
    [ordered]@{
        pairs = $Items.Count
        attributable_cpu_percent_median = Median $cpu
        attributable_cpu_percent_p10 = Percentile $cpu 0.10
        attributable_cpu_percent_p90 = Percentile $cpu 0.90
        process_cpu_percent_median = Median $proc
        p99_delta_ms_median = Median $p99
        p99_delta_ms_p90 = Percentile $p99 0.90
        cpu_target_percent = 2.0
        p99_target_ms = 0.15
        cpu_target_met = ((Median $cpu) -le 2.0)
        p99_target_met = ((Percentile $p99 0.90) -le 0.15)
    }
}

$warm = [math]::Min(3000, $Iterations)
foreach ($mode in @('baseline','light','full')) { $null = Invoke-Mode $mode $warm }

$raw = @()
$lightPairs = @()
$fullPairs = @()
for ($pair = 0; $pair -lt $Pairs; $pair++) {
    foreach ($mode in $(if (($pair % 2) -eq 0) { @('baseline','light') } else { @('light','baseline') })) {
        $m = Invoke-Mode $mode $Iterations
        $raw += [pscustomobject]@{ pair=$pair; comparison='light'; order=$mode; measurement=$m }
        if ($mode -eq 'baseline') { $baseLight = $m } else { $modeLight = $m }
        Start-Sleep -Milliseconds 150
    }
    $d = PairDelta $baseLight $modeLight
    $d | Add-Member -NotePropertyName pair -NotePropertyValue $pair
    $lightPairs += $d

    foreach ($mode in $(if (($pair % 2) -eq 0) { @('full','baseline') } else { @('baseline','full') })) {
        $m = Invoke-Mode $mode $Iterations
        $raw += [pscustomobject]@{ pair=$pair; comparison='full'; order=$mode; measurement=$m }
        if ($mode -eq 'baseline') { $baseFull = $m } else { $modeFull = $m }
        Start-Sleep -Milliseconds 150
    }
    $d = PairDelta $baseFull $modeFull
    $d | Add-Member -NotePropertyName pair -NotePropertyValue $pair
    $fullPairs += $d
}

$summary = [ordered]@{
    schema = 2
    iterations = $Iterations
    pairs = $Pairs
    affinity_mask = $AffinityMask
    methodology = 'paired AB/BA; equal warmup; producer thread CPU plus measured ARC background CPU; median and p90 across pairs'
    light = SummarizePairs $lightPairs
    full = SummarizePairs $fullPairs
    light_pairs = $lightPairs
    full_pairs = $fullPairs
}
$summary | ConvertTo-Json -Depth 10 | Set-Content traces/observer-benchmark-v2.json -Encoding utf8
$raw | ConvertTo-Json -Depth 8 | Set-Content traces/observer-benchmark-v2-raw.json -Encoding utf8

Write-Host ''
Write-Host '=== Observer CPU benchmark v2 ==='
[pscustomobject]@{
    mode='Light'
    cpu_median_percent=$summary.light.attributable_cpu_percent_median
    cpu_p90_percent=$summary.light.attributable_cpu_percent_p90
    p99_delta_median_ms=$summary.light.p99_delta_ms_median
    p99_delta_p90_ms=$summary.light.p99_delta_ms_p90
    cpu_target_met=$summary.light.cpu_target_met
    p99_target_met=$summary.light.p99_target_met
} | Format-Table -AutoSize
[pscustomobject]@{
    mode='Full'
    cpu_median_percent=$summary.full.attributable_cpu_percent_median
    cpu_p90_percent=$summary.full.attributable_cpu_percent_p90
    p99_delta_median_ms=$summary.full.p99_delta_ms_median
    p99_delta_p90_ms=$summary.full.p99_delta_ms_p90
    cpu_target_met=$summary.full.cpu_target_met
    p99_target_met=$summary.full.p99_target_met
} | Format-Table -AutoSize
Write-Host 'Saved traces/observer-benchmark-v2.json'
