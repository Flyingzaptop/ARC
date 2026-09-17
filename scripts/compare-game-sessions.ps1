param(
    [string]$Baseline,
    [string]$Candidate,
    [string]$SessionsRoot = "$env:USERPROFILE\Documents\ARC\Sessions"
)

$ErrorActionPreference = 'Stop'

function Find-Summary([string]$Value, [int]$Offset) {
    if ($Value) {
        if (Test-Path -LiteralPath $Value -PathType Container) { return Join-Path $Value 'summary.json' }
        return $Value
    }
    $items = @(Get-ChildItem -LiteralPath $SessionsRoot -Directory -ErrorAction Stop |
        Sort-Object LastWriteTime -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'summary.json') })
    if ($items.Count -le $Offset) { throw 'Not enough completed ARC sessions to compare.' }
    return Join-Path $items[$Offset].FullName 'summary.json'
}

$candidatePath = Find-Summary $Candidate 0
$baselinePath = Find-Summary $Baseline 1
$base = Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json
$cand = Get-Content -Raw -LiteralPath $candidatePath | ConvertFrom-Json

function Delta([string]$Name) {
    return [double]$cand.$Name - [double]$base.$Name
}

$rows = @(
    [pscustomobject]@{ Metric='Average FPS'; Baseline=[double]$base.average_fps; Candidate=[double]$cand.average_fps; Delta=(Delta 'average_fps'); Better='higher' },
    [pscustomobject]@{ Metric='1% low FPS'; Baseline=[double]$base.one_percent_low_fps; Candidate=[double]$cand.one_percent_low_fps; Delta=(Delta 'one_percent_low_fps'); Better='higher' },
    [pscustomobject]@{ Metric='0.1% low FPS'; Baseline=[double]$base.point_one_percent_low_fps; Candidate=[double]$cand.point_one_percent_low_fps; Delta=(Delta 'point_one_percent_low_fps'); Better='higher' },
    [pscustomobject]@{ Metric='P95 frame ms'; Baseline=[double]$base.p95_frame_ms; Candidate=[double]$cand.p95_frame_ms; Delta=(Delta 'p95_frame_ms'); Better='lower' },
    [pscustomobject]@{ Metric='P99 frame ms'; Baseline=[double]$base.p99_frame_ms; Candidate=[double]$cand.p99_frame_ms; Delta=(Delta 'p99_frame_ms'); Better='lower' },
    [pscustomobject]@{ Metric='Median display latency ms'; Baseline=[double]$base.median_display_latency_ms; Candidate=[double]$cand.median_display_latency_ms; Delta=(Delta 'median_display_latency_ms'); Better='lower' },
    [pscustomobject]@{ Metric='P95 display latency ms'; Baseline=[double]$base.p95_display_latency_ms; Candidate=[double]$cand.p95_display_latency_ms; Delta=(Delta 'p95_display_latency_ms'); Better='lower' },
    [pscustomobject]@{ Metric='Mean GPU busy ms'; Baseline=[double]$base.mean_gpu_busy_ms; Candidate=[double]$cand.mean_gpu_busy_ms; Delta=(Delta 'mean_gpu_busy_ms'); Better='context' },
    [pscustomobject]@{ Metric='Mean process CPU %'; Baseline=[double]$base.mean_process_cpu_total_percent; Candidate=[double]$cand.mean_process_cpu_total_percent; Delta=(Delta 'mean_process_cpu_total_percent'); Better='lower' },
    [pscustomobject]@{ Metric='Peak DXGI local usage MiB'; Baseline=[double]$base.peak_dxgi_local_usage_bytes / 1MB; Candidate=[double]$cand.peak_dxgi_local_usage_bytes / 1MB; Delta=((Delta 'peak_dxgi_local_usage_bytes') / 1MB); Better='lower' }
)

Write-Host "Baseline: $baselinePath" -ForegroundColor DarkGray
Write-Host "Candidate: $candidatePath" -ForegroundColor DarkGray
$rows | Format-Table -AutoSize -Property Metric,@{n='Baseline';e={[math]::Round($_.Baseline,3)}},@{n='Candidate';e={[math]::Round($_.Candidate,3)}},@{n='Delta';e={[math]::Round($_.Delta,3)}},Better

$out = [ordered]@{
    schema = 1
    baseline = $baselinePath
    candidate = $candidatePath
    created_utc = [DateTime]::UtcNow.ToString('o')
    metrics = $rows
}
$outPath = Join-Path (Split-Path -Parent $candidatePath) 'comparison-vs-baseline.json'
$out | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outPath -Encoding utf8
Write-Host "Comparison JSON: $outPath" -ForegroundColor Green
