param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetPid,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(5,60)][int]$Seconds=30,
    [ValidateRange(0,15)][int]$DelaySeconds=5
)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'CPU stack tracing requires an elevated launch confirmed by the user.' }
$target=Get-Process -Id $TargetPid -ErrorAction Stop
$created=$target.StartTime.ToUniversalTime().ToString('o')
$image=$target.Path
$folder=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $folder) { throw 'Fresh output directory required.' }
$profile=Join-Path $PSScriptRoot 'arc-cpu.wprp'
if(-not (Test-Path -LiteralPath $profile)) { throw 'Missing ARC CPU recording profile.' }
New-Item -ItemType Directory -Path $folder | Out-Null
$temp=Join-Path $folder 'recording'
New-Item -ItemType Directory -Path $temp | Out-Null
$instance='ARC_CPU_'+[Guid]::NewGuid().ToString('N')
$trace=Join-Path $folder 'cpu.etl'
$manifest=[ordered]@{schema=1; target_pid=$TargetPid; target_image=$image; target_creation_utc=$created; instance=$instance; seconds_requested=$Seconds; delay_seconds=$DelaySeconds; scope='System CPU scheduling and stack events; analysis must filter target PID and lifetime'; engine_adapter=$false; injected_dll=$false; overhead_measured=$false; event_loss_verified=$false; status='starting'}
$ownsSession=$false
try {
    if($DelaySeconds) { Start-Sleep -Seconds $DelaySeconds }
    $target.Refresh();if($target.HasExited){throw 'Target exited before capture.'}
    & wpr -start ($profile+'!ARCCPU') -filemode -recordtempto $temp -instancename $instance 2>&1 | Out-File (Join-Path $folder 'start.log')
    if($LASTEXITCODE -ne 0){throw 'WPR start failed; see start.log. No success is inferred from process launch.'}
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
    & wpr -stop $trace 'ARC CPU attribution' -instancename $instance 2>&1 | Out-File (Join-Path $folder 'stop.log')
    if($LASTEXITCODE -ne 0 -or -not(Test-Path -LiteralPath $trace)){throw 'WPR stop failed; see stop.log.'}
    $ownsSession=$false
    $manifest.status='recorded_pending_analysis';$manifest.trace_bytes=(Get-Item -LiteralPath $trace).Length
} catch {
    $manifest.status='failed';$manifest.error=$_.Exception.Message;throw
} finally {
    if($ownsSession){& wpr -cancel -instancename $instance 2>&1 | Out-File (Join-Path $folder 'cancel.log')}
    $manifest.finished_utc=[DateTime]::UtcNow.ToString('o')
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding utf8
}
