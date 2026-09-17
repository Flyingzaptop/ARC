param(
    [int]$Iterations = 15000,
    [int]$Pairs = 5,
    [UInt64]$AffinityMask = 0
)

$ErrorActionPreference = 'Stop'
if ($Pairs -lt 3 -or $Pairs -gt 15) { throw 'Pairs must be 3..15' }
if ($Iterations -lt 3000 -or $Iterations -gt 100000) { throw 'Iterations must be 3000..100000' }

$exe = Join-Path $PWD 'build/Release/dx12-mixed-resources.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe" }
New-Item -ItemType Directory -Force traces | Out-Null
Remove-Item -LiteralPath 'traces/observer-benchmark-v2-error.json' -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'traces/observer-benchmark-v2-checkpoint.json' -ErrorAction SilentlyContinue

function Invoke-Mode([string]$Mode, [int]$Count) {
    Write-Host ("benchmark-v2: mode={0} iterations={1}" -f $Mode, $Count)

    # The normal path deliberately uses the same direct invocation that the
    # already-proven Stage 2 benchmark uses. Start-Process adds extra native
    # process/PowerShell 5.1 behaviour that is irrelevant when no affinity is requested.
    if ($AffinityMask -eq 0) {
        & $exe $Mode $Count
        $code = $LASTEXITCODE
    } else {
        $processArgs = @($Mode, [string]$Count)
        $p = Start-Process -FilePath $exe -ArgumentList $processArgs -WorkingDirectory ([string]$PWD) -PassThru -NoNewWindow
        try { $p.ProcessorAffinity = [IntPtr]([Int64]$AffinityMask) }
        catch { Write-Warning "Could not set affinity: $($_.Exception.Message)" }
        $p.WaitForExit()
        $code = $p.ExitCode
    }

    if ($code -ne 0) { throw "Validation failed for $Mode/$Count with exit $code" }

    $metricsPath = "traces/dx12-mixed-resources-$Mode-metrics.json"
    $correctnessPath = "traces/dx12-mixed-resources-$Mode.json"
    if (-not (Test-Path -LiteralPath $metricsPath)) { throw "Missing benchmark metrics: $metricsPath" }
    if (-not (Test-Path -LiteralPath $correctnessPath)) { throw "Missing benchmark correctness: $correctnessPath" }

    $metrics = Get-Content -Raw -LiteralPath $metricsPath | ConvertFrom-Json
    $correctness = Get-Content -Raw -LiteralPath $correctnessPath | ConvertFrom-Json
    if ([int]$correctness.iterations -ne $Count) {
        throw "Stale benchmark output for $Mode: expected $Count iterations, got $($correctness.iterations)"
    }
    if (-not [bool]$correctness.valid -or [double]$correctness.dropped -ne 0) {
        throw "Observer correctness failed for $Mode/$Count: valid=$($correctness.valid) dropped=$($correctness.dropped)"
    }

    [pscustomobject]@{
        mode = $Mode
        iterations = $Count
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
    if (-not $Values -or -not $Values.Count) { return 0.0 }
    $v = @($Values | Sort-Object)
    $n = $v.Count
    if (($n % 2) -eq 1) { return [double]$v[[int]($n / 2)] }
    return ([double]$v[$n/2 - 1] + [double]$v[$n/2]) / 2.0
}

function Percentile([double[]]$Values, [double]$Q) {
    if (-not $Values -or -not $Values.Count) { return 0.0 }
    $v = @($Values | Sort-Object)
    if ($v.Count -eq 1) { return [double]$v[0] }
    $x = ($v.Count - 1) * $Q
    $lo = [int][math]::Floor($x)
    $hi = [int][math]::Ceiling($x)
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

function Save-Checkpoint([string]$Phase, [int]$CompletedPairs, $Raw, $LightPairs, $FullPairs) {
    [ordered]@{
        schema = 1
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        phase = $Phase
        requested_iterations = $Iterations
        requested_pairs = $Pairs
        completed_pairs = $CompletedPairs
        raw_measurements = $Raw
        light_pairs = $LightPairs
        full_pairs = $FullPairs
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath 'traces/observer-benchmark-v2-checkpoint.json' -Encoding utf8
}

$raw = @()
$lightPairs = @()
$fullPairs = @()
$currentPhase = 'initializing'
$currentMode = $null
$currentPair = -1

try {
    $warm = [math]::Min(3000, $Iterations)
    $currentPhase = 'warmup'
    foreach ($mode in @('baseline','light','full')) {
        $currentMode = $mode
        $null = Invoke-Mode $mode $warm
    }
    Save-Checkpoint 'warmup-complete' 0 $raw $lightPairs $fullPairs

    for ($pair = 0; $pair -lt $Pairs; $pair++) {
        $currentPair = $pair
        $currentPhase = 'light-pair'
        $baseLight = $null
        $modeLight = $null
        $lightOrder = if (($pair % 2) -eq 0) { @('baseline','light') } else { @('light','baseline') }
        foreach ($mode in $lightOrder) {
            $currentMode = $mode
            $m = Invoke-Mode $mode $Iterations
            $raw += [pscustomobject]@{ pair=$pair; comparison='light'; order=$mode; measurement=$m }
            if ($mode -eq 'baseline') { $baseLight = $m } else { $modeLight = $m }
            Save-Checkpoint ("pair-$pair-light-$mode") $pair $raw $lightPairs $fullPairs
            Start-Sleep -Milliseconds 150
        }
        if ($null -eq $baseLight -or $null -eq $modeLight) { throw "Incomplete light pair $pair" }
        $d = PairDelta $baseLight $modeLight
        $d | Add-Member -NotePropertyName pair -NotePropertyValue $pair
        $lightPairs += $d

        $currentPhase = 'full-pair'
        $baseFull = $null
        $modeFull = $null
        $fullOrder = if (($pair % 2) -eq 0) { @('full','baseline') } else { @('baseline','full') }
        foreach ($mode in $fullOrder) {
            $currentMode = $mode
            $m = Invoke-Mode $mode $Iterations
            $raw += [pscustomobject]@{ pair=$pair; comparison='full'; order=$mode; measurement=$m }
            if ($mode -eq 'baseline') { $baseFull = $m } else { $modeFull = $m }
            Save-Checkpoint ("pair-$pair-full-$mode") $pair $raw $lightPairs $fullPairs
            Start-Sleep -Milliseconds 150
        }
        if ($null -eq $baseFull -or $null -eq $modeFull) { throw "Incomplete full pair $pair" }
        $d = PairDelta $baseFull $modeFull
        $d | Add-Member -NotePropertyName pair -NotePropertyValue $pair
        $fullPairs += $d
        Save-Checkpoint ("pair-$pair-complete") ($pair + 1) $raw $lightPairs $fullPairs
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
    $summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath 'traces/observer-benchmark-v2.json' -Encoding utf8
    $raw | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'traces/observer-benchmark-v2-raw.json' -Encoding utf8
    Remove-Item -LiteralPath 'traces/observer-benchmark-v2-checkpoint.json' -ErrorAction SilentlyContinue

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
} catch {
    $message = $_.Exception.Message
    [ordered]@{
        schema = 1
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        phase = $currentPhase
        mode = $currentMode
        pair = $currentPair
        iterations = $Iterations
        affinity_mask = $AffinityMask
        error = $message
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath 'traces/observer-benchmark-v2-error.json' -Encoding utf8
    Save-Checkpoint ("failed-$currentPhase") ([math]::Max(0, $currentPair)) $raw $lightPairs $fullPairs
    Write-Host ("Observer benchmark v2 failed in phase '{0}', mode '{1}', pair {2}: {3}" -f $currentPhase, $currentMode, $currentPair, $message) -ForegroundColor Red
    throw
}
