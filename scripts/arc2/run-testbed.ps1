param(
    [Parameter(Mandatory=$true)][ValidateSet('wicked','cauldron','miniengine','diligent','bgfx')][string]$Testbed,
    [Parameter(Mandatory=$true)][string]$Workload,
    [Parameter(Mandatory=$true)][ValidateSet('native','passthrough','observe','optimize')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Exe,
    [string]$WorkingDirectory = '',
    [string[]]$Arguments = @(),
    [hashtable]$Environment = @{},
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [ValidateRange(0,1800)][int]$WarmupSeconds = 0,
    [ValidateRange(5,3600)][int]$Seconds = 30,
    [string]$FrontendDll = ''
)

$ErrorActionPreference = 'Stop'
function Read-GpuState {
    try {
        $line = & nvidia-smi.exe '--query-gpu=timestamp,temperature.gpu,clocks.current.graphics,clocks.current.memory,power.draw,utilization.gpu' '--format=csv,noheader,nounits' 2>$null | Select-Object -First 1
        if ($line) { return [string]$line }
        return $null
    } catch { return $null }
}
function Get-ArtifactHash([string]$Path) {
    try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
    catch { return $null }
}
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$exePath = (Resolve-Path -LiteralPath $Exe).Path
if (-not $WorkingDirectory) { $WorkingDirectory = Split-Path -Parent $exePath }
$workPath = (Resolve-Path -LiteralPath $WorkingDirectory).Path
$outPath = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outPath | Out-Null
$existingOutput = @(Get-ChildItem -LiteralPath $outPath -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'runner.log' })
if ($existingOutput.Count -gt 0) {
    throw "OutputDir must be fresh; existing artifact: $($existingOutput[0].FullName)"
}
$frontendPath = if ($FrontendDll) { [IO.Path]::GetFullPath($FrontendDll) } else { Join-Path $repo 'build\Release\arc2-frontend.dll' }
if ($Mode -ne 'native' -and -not (Test-Path -LiteralPath $frontendPath -PathType Leaf)) {
    throw "ARC2 frontend DLL absent: $frontendPath"
}
$exeShaBefore = Get-ArtifactHash $exePath
$frontendShaBefore = if ($Mode -eq 'native') { $null } else { Get-ArtifactHash $frontendPath }
$arcCommitBefore = (& git.exe -C $repo rev-parse HEAD).Trim()

$priorMode = $env:ARC2_MODE
$priorDll = $env:ARC2_DLL
$priorOutput = $env:ARC2_OUTPUT
$priorNativeCsv = $env:ARC2_NATIVE_FRAMES_CSV
$priorExtra = @{}
foreach ($key in $Environment.Keys) {
    $priorExtra[$key] = [Environment]::GetEnvironmentVariable([string]$key, 'Process')
}
$started = [DateTimeOffset]::UtcNow
$pidValue = $null
$exitCode = $null
$shutdown = 'not-started'
$failure = $null
$dumpStatus = 'not-requested'
$dumpFailure = $null
$tracePath = Join-Path $outPath 'arc2.json'
$nativeFramesPath = Join-Path $outPath 'native.frames.csv'
$qpcFrequency = [Diagnostics.Stopwatch]::Frequency
$qpcStarted = $null
$qpcMeasurementEnd = $null
$gpuBefore = $null
$gpuAfter = $null
try {
    $env:ARC2_NATIVE_FRAMES_CSV = $nativeFramesPath
    foreach ($key in $Environment.Keys) {
        [Environment]::SetEnvironmentVariable([string]$key, [string]$Environment[$key], 'Process')
    }
    if ($Mode -eq 'native') {
        Remove-Item Env:ARC2_MODE,Env:ARC2_DLL,Env:ARC2_OUTPUT -ErrorAction SilentlyContinue
    } else {
        $env:ARC2_MODE = $Mode
        $env:ARC2_DLL = $frontendPath
        $env:ARC2_OUTPUT = $tracePath
    }
    $gpuBefore = Read-GpuState
    $qpcStarted = [Diagnostics.Stopwatch]::GetTimestamp()
    $process = Start-Process -FilePath $exePath -ArgumentList $Arguments -WorkingDirectory $workPath `
        -RedirectStandardOutput (Join-Path $outPath 'stdout.log') `
        -RedirectStandardError (Join-Path $outPath 'stderr.log') -PassThru
    $pidValue = $process.Id
    $endedOnItsOwn = $process.WaitForExit($Seconds * 1000)
    $qpcMeasurementEnd = [Diagnostics.Stopwatch]::GetTimestamp()
    $gpuAfter = Read-GpuState
    if ($endedOnItsOwn) {
        $shutdown = 'self-exit'
    } else {
        if ($Mode -ne 'native') {
            try {
                & (Join-Path $PSScriptRoot 'dump-running-frontend.ps1') -ProcessId $process.Id -Dll $frontendPath -Output $tracePath
                $dumpStatus = 'success'
            } catch {
                $dumpStatus = 'failed'
                $dumpFailure = $_.Exception.Message
            }
        }
        $closed = $process.CloseMainWindow()
        if ($closed -and $process.WaitForExit(10000)) {
            $shutdown = 'window-close'
        } else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000) | Out-Null
            $shutdown = 'forced-timeout'
        }
    }
    if ($process.HasExited) { $exitCode = $process.ExitCode }
} catch {
    $failure = $_.Exception.Message
} finally {
    $env:ARC2_MODE = $priorMode
    $env:ARC2_DLL = $priorDll
    $env:ARC2_OUTPUT = $priorOutput
    $env:ARC2_NATIVE_FRAMES_CSV = $priorNativeCsv
    foreach ($key in $Environment.Keys) {
        [Environment]::SetEnvironmentVariable([string]$key, $priorExtra[$key], 'Process')
    }
}

$framesPath = "$tracePath.frames.csv"
$traceFresh = (Test-Path -LiteralPath $tracePath -PathType Leaf) -and
    ((Get-Item -LiteralPath $tracePath).LastWriteTimeUtc -ge $started.UtcDateTime.AddSeconds(-2))
$frontendFramesFresh = (Test-Path -LiteralPath $framesPath -PathType Leaf) -and
    ((Get-Item -LiteralPath $framesPath).LastWriteTimeUtc -ge $started.UtcDateTime.AddSeconds(-2))
$nativeFramesFresh = (Test-Path -LiteralPath $nativeFramesPath -PathType Leaf) -and
    ((Get-Item -LiteralPath $nativeFramesPath).LastWriteTimeUtc -ge $started.UtcDateTime.AddSeconds(-2))
$frameRows = if (Test-Path -LiteralPath $framesPath) {
    @(Import-Csv -LiteralPath $framesPath).Count
} else { 0 }
$nativeFrameRows = if (Test-Path -LiteralPath $nativeFramesPath) {
    @(Import-Csv -LiteralPath $nativeFramesPath).Count
} else { 0 }
$finished = [DateTimeOffset]::UtcNow
$exeShaAfter = Get-ArtifactHash $exePath
$frontendShaAfter = if ($Mode -eq 'native') { $null } else { Get-ArtifactHash $frontendPath }
$arcCommitAfter = (& git.exe -C $repo rev-parse HEAD).Trim()
$binaryHashStable = ($null -ne $exeShaBefore -and $exeShaBefore -eq $exeShaAfter -and
    ($Mode -eq 'native' -or ($null -ne $frontendShaBefore -and $frontendShaBefore -eq $frontendShaAfter)))
$repoHeadStable = $arcCommitBefore -eq $arcCommitAfter
$captureEnabled = @($Arguments | Where-Object { $_ -in @('--capture_path','--screenshot','-screenshot') }).Count -gt 0 -or $Environment.ContainsKey('ARC2_WICKED_SCREENSHOT') -or $Environment.ContainsKey('ARC2_WICKED_CAPTURE_DIR') -or $Environment.ContainsKey('ARC2_MINI_READBACK')
$usableRows = $nativeFrameRows -gt 0 -and ($Mode -eq 'native' -or $frameRows -gt 0)
$cleanClose = $exitCode -eq 0 -and $shutdown -in @('self-exit','window-close')
$dumpCompleted = $Mode -eq 'native' -or $dumpStatus -eq 'success' -or ($shutdown -eq 'self-exit' -and (Test-Path -LiteralPath $tracePath))
$result = [ordered]@{
    schema = 'arc2-testbed-run-v1'
    testbed = $Testbed; workload = $Workload; mode = $Mode
    executable = $exePath; executable_sha256 = $exeShaBefore
    executable_sha256_after = $exeShaAfter
    working_directory = $workPath; arguments = $Arguments; environment = $Environment
    arc_commit = $arcCommitBefore; arc_commit_after = $arcCommitAfter
    binary_hash_stable = $binaryHashStable; repo_head_stable = $repoHeadStable
    frontend_dll = if ($Mode -eq 'native') { $null } else { $frontendPath }
    frontend_sha256 = $frontendShaBefore
    frontend_sha256_after = $frontendShaAfter
    started_utc = $started.ToString('o'); finished_utc = $finished.ToString('o')
    qpc_frequency = $qpcFrequency; process_start_qpc = $qpcStarted
    warmup_anchor = 'parent QPC immediately before Start-Process; includes loader/startup time'
    gpu_state_query = 'timestamp,temperature.gpu,clocks.current.graphics,clocks.current.memory,power.draw,utilization.gpu (NVIDIA CSV, no units)'
    gpu_state_before = $gpuBefore; gpu_state_after = $gpuAfter
    measurement_start_qpc = if ($null -ne $qpcStarted) { $qpcStarted + $WarmupSeconds * $qpcFrequency } else { $null }
    measurement_end_qpc = $qpcMeasurementEnd; warmup_seconds = $WarmupSeconds
    pid = $pidValue; shutdown = $shutdown; exit_code = $exitCode
    arc_trace_exists = $traceFresh
    native_frames_fresh = $nativeFramesFresh; frontend_frames_fresh = $frontendFramesFresh
    present_rows = $frameRows; native_present_rows = $nativeFrameRows
    dump_status = $dumpStatus; dump_failure = $dumpFailure
    capture_enabled = $captureEnabled
    eligible_for_timing_analysis = ($binaryHashStable -and $repoHeadStable -and
        $cleanClose -and $dumpCompleted -and $nativeFramesFresh -and
        ($Mode -eq 'native' -or ($traceFresh -and $frontendFramesFresh)) -and
        $usableRows -and -not $captureEnabled -and $Seconds -gt $WarmupSeconds)
    failure = $failure
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outPath 'run.json') -Encoding utf8
$result | ConvertTo-Json -Depth 5
if ($failure -or $shutdown -eq 'forced-timeout' -or $exitCode -ne 0) { exit 1 }
