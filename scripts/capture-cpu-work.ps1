param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetPid,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(5,60)][int]$Seconds=30,
    [ValidateRange(0,15)][int]$DelaySeconds=5,
    [ValidateRange(1,64)][int]$MaxExportGiB=8,
    [switch]$SkipExport
)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'CPU stack tracing requires elevation.' }
$target=Get-Process -Id $TargetPid -ErrorAction Stop
$folder=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $folder) { throw 'Fresh output directory required.' }
$profile=Join-Path $PSScriptRoot 'arc-cpu.wprp'
$xperf=(Get-Command xperf.exe -ErrorAction SilentlyContinue).Source
if(-not $xperf) { throw 'Windows Performance Toolkit xperf.exe is required.' }
New-Item -ItemType Directory -Path $folder | Out-Null
$temp=Join-Path $folder 'recording'
New-Item -ItemType Directory -Path $temp | Out-Null
$instance='ARC_CPU_'+[Guid]::NewGuid().ToString('N')
$trace=Join-Path $folder 'cpu.etl'
$manifest=[ordered]@{
    schema=2; target_pid=$TargetPid; target_image=$target.Path
    target_creation_utc=$target.StartTime.ToUniversalTime().ToString('o')
    instance=$instance; seconds_requested=$Seconds; delay_seconds=$DelaySeconds
    scope='System ETW CPU scheduling, image lifetime and presentation evidence; filter by target PID and lifetime'
    clock='One ETL PerfCounter clock; xperf event timestamps are microseconds from trace start UTC in tracestats'
    engine_adapter=$false; injected_dll=$false; overhead_measured=$false
    event_loss_verified=$false; status='starting'
}
$ownsSession=$false
try {
    if($DelaySeconds) { Start-Sleep -Seconds $DelaySeconds }
    $target.Refresh();if($target.HasExited){throw 'Target exited before capture.'}
    & wpr -start ($profile+'!ARCCPU') -filemode -recordtempto $temp -instancename $instance 2>&1 | Out-File (Join-Path $folder 'start.log')
    if($LASTEXITCODE -ne 0){throw 'WPR start failed; see start.log.'}
    $ownsSession=$true
    $manifest.status='recording';$manifest.started_utc=[DateTime]::UtcNow.ToString('o')
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding utf8
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.Elapsed.TotalSeconds -lt $Seconds){
        Start-Sleep -Milliseconds 500
        $target.Refresh();if($target.HasExited){$manifest.target_exited=$true;break}
        $bytes=(Get-ChildItem -LiteralPath $temp -File -Recurse | Measure-Object -Property Length -Sum).Sum
        if($bytes -ge 512MB){$manifest.disk_budget_stopped=$true;break}
    }
    $manifest.seconds_recorded=$clock.Elapsed.TotalSeconds
    & wpr -stop $trace 'ARC synchronized CPU and presentation attribution' -instancename $instance 2>&1 | Out-File (Join-Path $folder 'stop.log')
    if($LASTEXITCODE -ne 0 -or -not(Test-Path -LiteralPath $trace)){throw 'WPR stop failed; see stop.log.'}
    $ownsSession=$false
    $manifest.status='recorded_pending_analysis';$manifest.trace_bytes=(Get-Item -LiteralPath $trace).Length
    & $xperf -i $trace -o (Join-Path $folder 'trace-stats.txt') -a tracestats 2>&1 | Out-File (Join-Path $folder 'stats.log')
    if($LASTEXITCODE -ne 0){throw 'xperf tracestats failed; event loss is unknown.'}
    $stats=Get-Content -LiteralPath (Join-Path $folder 'trace-stats.txt') -Raw
    $lostBuffers=[regex]::Match($stats,'Total # Lost Buffers\s*:\s*(\d+)')
    $lostEvents=[regex]::Match($stats,'Total # Lost Events\s*:\s*(\d+)')
    $manifest.event_loss_verified=($lostBuffers.Success -and $lostEvents.Success)
    if($lostBuffers.Success){$manifest.lost_buffers=[int]$lostBuffers.Groups[1].Value}
    if($lostEvents.Success){$manifest.lost_events=[int]$lostEvents.Groups[1].Value}
    if(-not $SkipExport){
        $dump=Join-Path $folder 'events.txt'
        $exportArgs=@('-i',('"'+$trace+'"'),'-o',('"'+$dump+'"'),'-a','dumper')
        $export=Start-Process -FilePath $xperf -ArgumentList $exportArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'export.log') -RedirectStandardError (Join-Path $folder 'export-errors.log')
        while(-not $export.HasExited){
            Start-Sleep -Milliseconds 500
            $export.Refresh()
            if((Test-Path -LiteralPath $dump) -and (Get-Item -LiteralPath $dump).Length -gt ([long]$MaxExportGiB*1GB)){
                Stop-Process -Id $export.Id -Force -ErrorAction SilentlyContinue
                throw 'xperf text export exceeded MaxExportGiB; ETL remains available for a shorter ranged export.'
            }
        }
        if($export.ExitCode -ne 0){throw 'xperf dumper failed; see export-errors.log.'}
        $manifest.export_bytes=(Get-Item -LiteralPath $dump).Length
        & python (Join-Path $PSScriptRoot 'analyze-cpu-regions.py') --capture $folder 2>&1 | Out-File (Join-Path $folder 'analysis.log')
        if($LASTEXITCODE -ne 0){throw 'CPU region analysis failed; see analysis.log.'}
        $manifest.status='analyzed'
    }
} catch {
    $manifest.status='failed';$manifest.error=$_.Exception.Message;throw
} finally {
    if($ownsSession){& wpr -cancel -instancename $instance 2>&1 | Out-File (Join-Path $folder 'cancel.log')}
    $manifest.finished_utc=[DateTime]::UtcNow.ToString('o')
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding utf8
}
