param(
    [string]$SourceBranch = 'dev/stage4-live-runtime',
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage4Validation",
    [ValidateRange(3, 30)][int]$Stage2Rounds = 6,
    [ValidateRange(3000, 100000)][int]$ObserverIterations = 15000,
    [ValidateRange(3, 15)][int]$ObserverPairs = 5,
    [switch]$Quick
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Is-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Vs-Path {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $path) { return $null }
    return ($path | Select-Object -First 1)
}
function Has-GraphicsTools {
    try {
        $cap = Get-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' -ErrorAction Stop
        return $cap.State -eq 'Installed'
    } catch { return $false }
}
function Relaunch-Admin {
    $launchArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
        '-SourceBranch', ('"' + $SourceBranch + '"'),
        '-RepoUrl', ('"' + $RepoUrl + '"'),
        '-WorkRoot', ('"' + $WorkRoot + '"'),
        '-Stage2Rounds', $Stage2Rounds,
        '-ObserverIterations', $ObserverIterations,
        '-ObserverPairs', $ObserverPairs
    )
    if ($Quick) { $launchArgs += '-Quick' }
    Write-Host 'ARC needs one UAC prompt to install missing Windows prerequisites.' -ForegroundColor Yellow
    $proc = Start-Process powershell.exe -Verb RunAs -ArgumentList $launchArgs -Wait -PassThru
    exit $proc.ExitCode
}
function Ensure-Prereqs {
    if ($env:OS -ne 'Windows_NT') { throw 'Windows is required.' }
    $needGit = -not (Get-Command git.exe -ErrorAction SilentlyContinue)
    $needVs = -not (Vs-Path)
    $needGraphics = -not (Has-GraphicsTools)
    if (($needGit -or $needVs -or $needGraphics) -and -not (Is-Admin)) { Relaunch-Admin }

    if ($needGit) {
        Step 'Installing Git for Windows'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'Git is missing and winget is unavailable.' }
        & winget.exe install --id Git.Git --exact --silent --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { throw "Git installation failed: $LASTEXITCODE" }
        $env:Path = 'C:\Program Files\Git\cmd;C:\Program Files\Git\bin;' + $env:Path
    }
    if (-not (Vs-Path)) {
        Step 'Installing Visual Studio C++ Build Tools'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'MSVC Build Tools are missing and winget is unavailable.' }
        & winget.exe install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-source-agreements --accept-package-agreements --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        if ($LASTEXITCODE -ne 0) { throw "Build Tools installation failed: $LASTEXITCODE" }
    }
    if (-not (Has-GraphicsTools)) {
        Step 'Installing Windows Graphics Tools'
        $result = Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0'
        if ($result.RestartNeeded) { throw 'Windows requests a reboot after Graphics Tools installation. Reboot and run the same command again.' }
        if (-not (Has-GraphicsTools)) { throw 'Graphics Tools installation failed.' }
    }
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is unavailable.' }
    if (-not (Vs-Path)) { throw 'MSVC Build Tools are unavailable.' }
}

function Publish-FailureBundle([string]$RepoDir, [string]$SourceSha, [string]$Stamp, [string]$LogPath) {
    Step 'Publishing failure diagnostics'
    Push-Location $RepoDir
    try {
        $branch = "results/stage4-failure-$Stamp"
        $destination = "results/stage4-failure/$Stamp"
        $savedPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $switchOutput = & git.exe switch -c $branch $SourceSha 2>&1
        $switchCode = $LASTEXITCODE
        $ErrorActionPreference = $savedPreference
        if ($switchCode -ne 0) { throw ("Could not create failure-results branch: " + ($switchOutput -join ' ')) }
        New-Item -ItemType Directory -Force $destination | Out-Null
        if (Test-Path $LogPath) { Copy-Item -LiteralPath $LogPath -Destination (Join-Path $destination 'validation.log') -Force }
        $traces = Join-Path $RepoDir 'traces'
        if (Test-Path $traces) {
            Get-ChildItem -LiteralPath $traces -File | Where-Object { $_.Extension -in @('.json','.csv','.md','.txt') } | ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            }
        }
        [ordered]@{
            schema = 1
            source_commit = $SourceSha
            source_branch = $SourceBranch
            created_utc = [DateTime]::UtcNow.ToString('o')
            status = 'FAILED_BEFORE_NORMAL_PUBLICATION'
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'failure-manifest.json') -Encoding utf8
        & git.exe add -- $destination
        if ($LASTEXITCODE -ne 0) { throw 'Could not stage failure diagnostics.' }
        & git.exe commit -m "Publish Stage 4 failure diagnostics $Stamp" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Could not commit failure diagnostics.' }
        & git.exe push -u origin $branch | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Could not push failure diagnostics.' }
        $remoteUrl = (& git.exe remote get-url origin).Trim()
        if ($remoteUrl -match '^git@github.com:(.+)\.git$') { $remoteUrl = "https://github.com/$($Matches[1])" }
        elseif ($remoteUrl -match '^https://github.com/(.+)\.git$') { $remoteUrl = "https://github.com/$($Matches[1])" }
        return "$remoteUrl/tree/$branch/$destination"
    } finally {
        $savedPreference = $ErrorActionPreference
        $ErrorActionPreference = 'SilentlyContinue'
        & git.exe switch $SourceBranch *> $null
        $ErrorActionPreference = $savedPreference
        Pop-Location
    }
}

Ensure-Prereqs
$repoDir = Join-Path $WorkRoot 'repo'
$logsDir = Join-Path $WorkRoot 'logs'
New-Item -ItemType Directory -Force $WorkRoot, $logsDir | Out-Null

Step 'Preparing clean ARC Stage 4 checkout'
if (-not (Test-Path (Join-Path $repoDir '.git'))) {
    if (Test-Path $repoDir) { Remove-Item -Recurse -Force $repoDir }
    & git.exe clone $RepoUrl $repoDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed: $LASTEXITCODE" }
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$logPath = Join-Path $logsDir ("stage4-$stamp.log")
$exitCode = 1
$sourceSha = $null
$normalResultsUrl = $null
$fallbackResultsUrl = $null
$transcriptStarted = $false

Push-Location $repoDir
try {
    & git.exe remote set-url origin $RepoUrl
    & git.exe fetch origin $SourceBranch --prune
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed: $LASTEXITCODE" }
    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe switch -C $SourceBranch ("origin/" + $SourceBranch)
    if ($LASTEXITCODE -ne 0) { throw "git switch failed: $LASTEXITCODE" }
    $sourceSha = (& git.exe rev-parse HEAD).Trim()

    Step ("Running complete Stage 4 validation on " + $sourceSha)
    $childArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repoDir 'scripts\stage4-validate.ps1'),
        '-Stage2Rounds', [string]$Stage2Rounds,
        '-ObserverIterations', [string]$ObserverIterations,
        '-ObserverPairs', [string]$ObserverPairs,
        '-PublishResults'
    )
    if ($Quick) { $childArgs += '-Quick' }

    Start-Transcript -LiteralPath $logPath -Force | Out-Null
    $transcriptStarted = $true
    try {
        & powershell.exe @childArgs
        $exitCode = $LASTEXITCODE
    } finally {
        if ($transcriptStarted) {
            Stop-Transcript | Out-Null
            $transcriptStarted = $false
        }
    }

    $urlFile = Join-Path $repoDir 'traces\stage4-results-url.txt'
    if (Test-Path $urlFile) { $normalResultsUrl = (Get-Content -Raw -LiteralPath $urlFile).Trim() }
    if ($exitCode -ne 0 -and -not $normalResultsUrl) {
        throw "Stage 4 validation process exited with code $exitCode"
    }
} catch {
    if ($transcriptStarted) {
        try { Stop-Transcript | Out-Null } catch {}
        $transcriptStarted = $false
    }
    $errorLine = 'STAGE4 BOOTSTRAP ERROR: ' + $_.Exception.Message
    $errorLine | Add-Content -LiteralPath $logPath -Encoding utf8
    Write-Host $errorLine -ForegroundColor Red
    $exitCode = 1
} finally {
    Pop-Location
}

if (-not $normalResultsUrl -and $sourceSha) {
    try { $fallbackResultsUrl = Publish-FailureBundle $repoDir $sourceSha $stamp $logPath }
    catch { Write-Host ("Failure diagnostics could not be pushed: " + $_.Exception.Message) -ForegroundColor Red }
}

Write-Host ''
Write-Host '==============================================' -ForegroundColor Cyan
Write-Host ("Stage 4 source SHA: " + $(if ($sourceSha) { $sourceSha } else { 'unknown' }))
Write-Host ("Validation exit code: " + $exitCode)
Write-Host ("Local log: " + $logPath)
if ($normalResultsUrl) { Write-Host ("Results URL: " + $normalResultsUrl) -ForegroundColor Green }
elseif ($fallbackResultsUrl) { Write-Host ("Failure results URL: " + $fallbackResultsUrl) -ForegroundColor Yellow }
else { Write-Host 'No remote result URL was created; local log is preserved.' -ForegroundColor Red }
Write-Host '=============================================='
exit $exitCode
