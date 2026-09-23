param(
    [string]$Executable,
    [string[]]$ApplicationArguments=@(),
    [ValidateSet('baseline','study','neutral','apply')][string]$Mode='study',
    [ValidateSet('auto','specialize','memo','incremental')][string]$Actuator='auto',
    [string]$Output,
    [string]$DynamoRoot=(Join-Path $PSScriptRoot '../runtime/dynamorio'),
    [string]$Client=(Join-Path $PSScriptRoot '../bin/arc_cpu_client.dll'),
    [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
# Quote argv using the Windows CommandLineToArgvW/MSVC convention. Never use a shell.
function ConvertTo-NativeArgument([string]$Value) {
    if($Value -notmatch '[\s"]' -and $Value.Length){return $Value}
    $result='"';$slashes=0
    foreach($c in $Value.ToCharArray()) {
        if($c -eq '\'){$slashes++;continue}
        if($c -eq '"'){$result+=('\' * (2*$slashes+1))+'"'}
        else {$result+=('\' * $slashes)+$c}
        $slashes=0
    }
    return $result+('\' * (2*$slashes))+'"'
}
if(-not $Executable){
    if($ValidateOnly){throw 'Executable is required for validation.'}
    Add-Type -AssemblyName System.Windows.Forms
    $picker=New-Object System.Windows.Forms.OpenFileDialog
    $picker.Filter='Windows executable (*.exe)|*.exe';$picker.Title='ARC CPU: select executable'
    if($picker.ShowDialog() -ne 'OK'){return}
    $Executable=$picker.FileName
}
$exe=[IO.Path]::GetFullPath($Executable)
$clientPath=[IO.Path]::GetFullPath($Client)
$runner=[IO.Path]::GetFullPath((Join-Path $DynamoRoot 'bin64/drrun.exe'))
$requiredFiles=@($exe)
if($Mode -ne 'baseline'){$requiredFiles+=@($clientPath,$runner)}
foreach($required in $requiredFiles) {
    if(-not(Test-Path -LiteralPath $required -PathType Leaf)){throw "Missing required file: $required"}
}
if([IO.Path]::GetExtension($exe) -ne '.exe'){throw 'Target must be a Windows executable.'}
if(-not $Output){$Output=Join-Path $PSScriptRoot ('../sessions/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))}
$folder=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $folder){throw 'Use a fresh output directory to preserve previous evidence.'}
$stop=Join-Path $folder 'stop.control'
$report=Join-Path $folder 'cpu-runtime.json'
$pidFile=Join-Path $folder 'target.pid'
$argv=@('-pidfile',$pidFile,'-no_follow_children','-msgbox_mask','0','-stderr_mask','15',
    '-cache_bb_max','1M','-cache_trace_max','1M','-cache_shared_bb_max','64M','-cache_shared_trace_max','32M',
    '-c',$clientPath,'-mode',$Mode,'-actuator',$Actuator,'-out',$report,'-stop',$stop,'--',$exe)+$ApplicationArguments
if($Mode -eq 'baseline'){$runner=$exe;$argv=$ApplicationArguments}
$nativeArguments=($argv | ForEach-Object {ConvertTo-NativeArgument $_}) -join ' '
if($ValidateOnly){[ordered]@{runner=$runner;arguments=$nativeArguments;output=$folder;mode=$Mode;actuator=$Actuator}|ConvertTo-Json;return}
New-Item -ItemType Directory -Path $folder | Out-Null
[IO.File]::WriteAllBytes($stop,[byte[]]@(0,0,0,0))
$manifest=[ordered]@{
    schema=1;mode=$Mode;actuator=$Actuator;status='starting';started_utc=[DateTime]::UtcNow.ToString('o')
    target=$exe;target_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    client_sha256=$(if($Mode -ne 'baseline'){(Get-FileHash -LiteralPath $clientPath -Algorithm SHA256).Hash}else{$null})
    runner_sha256=(Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash
    graphics_dll_requested=$false;cpu_backend=$(if($Mode -eq 'baseline'){'none'}else{'DynamoRIO'});exclusive_cpu_session=$true
    native_resolution_unchanged=$true;dbi_overhead='unmeasured';game_benefit='unknown'
    stop_request=$stop;report=$report;arguments=$ApplicationArguments;follow_children=$false
    target_pid=$null;target_creation_utc=$null;target_identity_status='pending'
    code_cache_limits=@{private_bb_mib=1;private_trace_mib=1;shared_bb_mib=64;shared_trace_mib=32;scope='code caches only; metadata and per-thread memory additional'}
}
$manifestPath=Join-Path $folder 'session.json'
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $manifestPath -Encoding UTF8
# A stop request disables new application work; it does not kill the application.
$stopScript=@'
$ErrorActionPreference='Stop'
$path=Join-Path $PSScriptRoot 'stop.control'
$mapping=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile($path,[IO.FileMode]::Open)
try {
    $view=$mapping.CreateViewAccessor(0,4)
    try {$view.Write([long]0,[int]1);$view.Flush()} finally {$view.Dispose()}
} finally {$mapping.Dispose()}
'@
Set-Content -LiteralPath (Join-Path $folder 'stop.ps1') -Encoding UTF8 -Value $stopScript
Set-Content -LiteralPath (Join-Path $folder 'STOP-ARC.cmd') -Encoding ASCII -Value '@echo off', 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop.ps1"'
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=$runner;$info.Arguments=$nativeArguments
$info.WorkingDirectory=Split-Path -Parent $exe;$info.UseShellExecute=$false
$info.RedirectStandardError=$true
$process=New-Object Diagnostics.Process
$process.StartInfo=$info
try {
    if(-not $process.Start()){throw 'DynamoRIO process did not start.'}
    $errorRead=$process.StandardError.ReadToEndAsync()
    if($Mode -eq 'baseline') {
        $manifest.target_pid=$process.Id
        $manifest.target_creation_utc=$process.StartTime.ToUniversalTime().ToString('o')
        $manifest.target_identity_status='direct_process_handle'
    }
    $manifest.runner_pid=$process.Id;$manifest.status='running'
    $manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host "ARC CPU $Mode. Evidence: $folder"
    Write-Host "To stop ARC actions without closing the application: $folder/STOP-ARC.cmd"
    do {
        if($manifest.target_identity_status -eq 'pending') {
            $targetId=$null
            if($Mode -eq 'baseline'){$targetId=$process.Id}
            elseif(Test-Path -LiteralPath $pidFile){
                $text=(Get-Content -LiteralPath $pidFile -Raw).Trim()
                if($text -match '^\d+$'){$targetId=[int]$text}
            }
            if($targetId){
                $manifest.target_pid=$targetId
                try {
                    $targetProcess=Get-Process -Id $targetId -ErrorAction Stop
                    $manifest.target_creation_utc=$targetProcess.StartTime.ToUniversalTime().ToString('o')
                    $manifest.target_identity_status=if($targetProcess.Path -eq $exe){'verified'}else{'image_mismatch'}
                    $targetProcess.Dispose()
                } catch {$manifest.target_identity_status='unavailable_process_exited_or_access_denied'}
                $manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $manifestPath -Encoding UTF8
            }
        }
    } while(-not $process.WaitForExit(100))
    $errorRead.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $folder 'process-errors.log') -Encoding UTF8
    $manifest.exit_code=$process.ExitCode
    $manifest.status=if($process.ExitCode -eq 0 -and ($Mode -eq 'baseline' -or (Test-Path -LiteralPath $report))){'finished_pending_analysis'}else{'failed_or_missing_evidence'}
} catch {$manifest.status='failed';$manifest.error=$_.Exception.Message;throw}
finally {
    $manifest.finished_utc=[DateTime]::UtcNow.ToString('o')
    $manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $process.Dispose()
}
