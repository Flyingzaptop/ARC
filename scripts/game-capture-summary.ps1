param(
    [Parameter(Mandatory=$true)][string]$CaptureDir,
    [string]$Mode = 'unknown'
)

$ErrorActionPreference = 'Stop'
$csvFiles = @(Get-ChildItem -LiteralPath $CaptureDir -Filter '*.csv' -File -ErrorAction SilentlyContinue)
if (-not $csvFiles) {
    [ordered]@{ schema=1; mode=$Mode; valid=$false; reason='no_presentmon_csv' } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $CaptureDir 'summary.json') -Encoding utf8
    exit 0
}

$rows = @()
foreach ($file in $csvFiles) { $rows += @(Import-Csv -LiteralPath $file.FullName) }
if (-not $rows) { throw 'PresentMon CSV contains no rows.' }

$properties = @($rows[0].PSObject.Properties.Name)
$frameColumn = @('FrameTime','MsBetweenPresents','DisplayedTime') | Where-Object { $properties -contains $_ } | Select-Object -First 1
if (-not $frameColumn) { throw ('No supported frame-time column found. Columns: ' + ($properties -join ', ')) }

$values = @($rows | ForEach-Object {
    $v = 0.0
    if ([double]::TryParse([string]$_.$frameColumn, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v) -and $v -gt 0 -and $v -lt 10000) { $v }
})
if ($values.Count -lt 30) { throw "Too few valid frame samples: $($values.Count)" }

$sorted = @($values | Sort-Object)
function Percentile([double[]]$Data, [double]$P) {
    if ($Data.Count -eq 0) { return $null }
    $index = [Math]::Ceiling($P * $Data.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($Data.Count - 1, $index))
    return [double]$Data[$index]
}
$mean = ($values | Measure-Object -Average).Average
$median = Percentile $sorted 0.50
$p95 = Percentile $sorted 0.95
$p99 = Percentile $sorted 0.99
$stutterThreshold = [Math]::Max(33.333, 2.0 * $median)
$stutters = @($values | Where-Object { $_ -gt $stutterThreshold }).Count

$monitorPath = Join-Path $CaptureDir 'dxgi-monitor.json'
$dxgi = if (Test-Path $monitorPath) { Get-Content -Raw -LiteralPath $monitorPath | ConvertFrom-Json } else { $null }

$summary = [ordered]@{
    schema = 1
    valid = $true
    mode = $Mode
    csv_files = $csvFiles.Count
    frame_samples = $values.Count
    frame_time_column = $frameColumn
    average_fps = if ($mean -gt 0) { 1000.0 / $mean } else { $null }
    one_percent_low_fps = if ($p99 -gt 0) { 1000.0 / $p99 } else { $null }
    median_frame_ms = $median
    p95_frame_ms = $p95
    p99_frame_ms = $p99
    stutter_threshold_ms = $stutterThreshold
    stutter_count = $stutters
    stutter_percent = 100.0 * $stutters / $values.Count
    dxgi = $dxgi
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $CaptureDir 'summary.json') -Encoding utf8
Write-Host ('Average FPS: {0:N1} | 1% low: {1:N1} | P99: {2:N2} ms | stutters: {3}' -f $summary.average_fps,$summary.one_percent_low_fps,$summary.p99_frame_ms,$summary.stutter_count)
