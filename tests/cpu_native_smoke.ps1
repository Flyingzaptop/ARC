param(
    [Parameter(Mandatory=$true)][string]$DynamoRIO,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\cpu_backend'),
    [string]$EvidenceDir = (Join-Path $PSScriptRoot '..\build\cpu_backend\evidence-final')
)
$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildDir)
$evidence = [IO.Path]::GetFullPath($EvidenceDir)
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$dr = Join-Path $DynamoRIO 'bin64\drrun.exe'
$client = Join-Path $build 'Release\arc_cpu_client.dll'
$fixture = Join-Path $build 'Release\cpu_native_fixture.exe'
$stop = Join-Path $evidence 'stop.control'
$records = @()

function Invoke-CpuCase([string]$name, [string]$mode, [string]$actuator, [string[]]$fixtureArgs) {
    $stdout = Join-Path $evidence "$name.stdout.txt"
    $json = Join-Path $evidence "$name.json"
    $args = @('-msgbox_mask','0','-stderr_mask','15','-no_follow_children',
              '-c',$client,'-mode',$mode,'-actuator',$actuator,'-out',$json)
    if ($name -eq 'stop') {
        [IO.File]::WriteAllBytes($stop,[byte[]](0,0,0,0))
        $args += @('-stop',$stop)
    }
    $args += @('--',$fixture)
    $args += $fixtureArgs
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $dr @args 2>&1 | Out-File -LiteralPath $stdout -Encoding utf8
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $run = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    $script:records += [pscustomobject]@{
        case = $name; exit_code = $exitCode; wall_ms = $watch.Elapsed.TotalMilliseconds
        pid = $run.pid; calls = $run.calls; executions = $run.executions
        stopped = $run.stopped; stop_control_available = $run.stop_control_available
        protection_events = $run.protection_events; stdout = $stdout; json = $json
        command = "$dr $($args -join ' ')"
    }
    if ($exitCode -ne 0) { throw "CPU smoke $name failed; see $stdout" }
    if ($name -in @('neutral','study','auto') -and $run.executions -ne 0) { throw "$name unexpectedly changed application work" }
    if ($name -eq 'study' -and $run.capture_completed -lt 1) { throw 'No complete bounded CPU study' }
    if ($name -in @('specialize','memo','incremental','protect','stop') -and $run.executions -lt 1) { throw "$name never executed its actuator" }
    if ($name -eq 'specialize' -and $run.guard_misses -lt 1) { throw 'Specialization fallback untested' }
    if ($name -eq 'incremental' -and -not @($run.regions | Where-Object {$_.dirty_nodes -gt 0 -and $_.reused_nodes -gt 0}).Count) { throw 'No partial incremental recomputation' }
    if ($name -eq 'stop' -and (-not $run.stopped -or -not $run.stop_control_available)) { throw 'Stop was not acknowledged' }
    if ($name -in @('stop','protect') -and @($run.regions | Where-Object {$_.active}).Count) { throw "$name left an active publication" }
}

$watch = [Diagnostics.Stopwatch]::StartNew()
& $fixture 2>&1 | Out-File -LiteralPath (Join-Path $evidence 'original.stdout.txt') -Encoding utf8
$originalExit = $LASTEXITCODE
$watch.Stop()
$records += [pscustomobject]@{case='original';exit_code=$originalExit;wall_ms=$watch.Elapsed.TotalMilliseconds;
    stdout=(Join-Path $evidence 'original.stdout.txt');command=$fixture}
if ($originalExit -ne 0) { throw 'Original CPU fixture failed' }
Invoke-CpuCase 'neutral' 'neutral' 'auto' @()
Invoke-CpuCase 'study' 'study' 'auto' @()
Invoke-CpuCase 'auto' 'apply' 'auto' @()
Invoke-CpuCase 'specialize' 'apply' 'specialize' @()
Invoke-CpuCase 'memo' 'apply' 'memo' @()
Invoke-CpuCase 'incremental' 'apply' 'incremental' @()
Invoke-CpuCase 'protect' 'apply' 'incremental' @('--protect')
Invoke-CpuCase 'stop' 'apply' 'incremental' @('--stop',$stop)

$summary = [pscustomobject]@{
    utc = [DateTime]::UtcNow.ToString('o')
    dynamorio = $DynamoRIO
    dynamorio_zip_sha256 = '5516B8B4C929885DC4172E088ED11EA5DB967F80ED739A3E7925FE2CDD798C6D'
    client_sha256 = (Get-FileHash $client -Algorithm SHA256).Hash
    fixture_sha256 = (Get-FileHash $fixture -Algorithm SHA256).Hash
    runs = $records
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'summary.json') -Encoding utf8
$records | Format-Table case,exit_code,wall_ms,calls,executions,stopped,stop_control_available,protection_events -AutoSize
