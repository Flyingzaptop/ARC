param(
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][string]$ProcessName,
    [Parameter(Mandatory=$true)][string]$CaptureDir,
    [ValidateRange(15,3600)][int]$Seconds = 120,
    [ValidateRange(0,120)][int]$DelaySeconds = 8,
    [ValidateSet('baseline','arc-observe')][string]$Mode = 'arc-observe',
    [string]$MonitorExe = ''
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
$meta = [ordered]@{
    schema = 2
    mode = $Mode
    process_name = $ProcessName
    seconds = $Seconds
    delay_seconds = $DelaySeconds
    started_utc = [DateTime]::UtcNow.ToString('o')
    presentmon = $PresentMon
    arc_external_monitor_enabled = ($Mode -eq 'arc-observe')
}
$meta | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $CaptureDir 'session.json') -Encoding utf8

if ($Mode -eq 'arc-observe' -and $MonitorExe -and (Test-Path -LiteralPath $MonitorExe)) {
    Start-Process -FilePath $MonitorExe -ArgumentList @('--seconds',[string]($Seconds + $DelaySeconds),'--interval-ms','100','--output',(Join-Path $CaptureDir 'dxgi-monitor.json')) -WindowStyle Hidden | Out-Null
}

$csv = Join-Path $CaptureDir 'presentmon.csv'
$args = @(
    '--process_name', $ProcessName,
    '--output_file', $csv,
    '--multi_csv',
    '--delay', [string]$DelaySeconds,
    '--timed', [string]$Seconds,
    '--terminate_after_timed',
    '--no_console_stats',
    '--v2_metrics'
)

try {
    $proc = Start-Process -FilePath $PresentMon -ArgumentList $args -PassThru -Wait
    $meta.presentmon_exit_code = $proc.ExitCode
} catch {
    $meta.presentmon_error = $_.Exception.Message
}
$meta.finished_utc = [DateTime]::UtcNow.ToString('o')
$meta | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $CaptureDir 'session.json') -Encoding utf8

& (Join-Path $PSScriptRoot 'game-capture-summary.ps1') -CaptureDir $CaptureDir -Mode $Mode
Write-Host "ARC capture complete: $CaptureDir"
