param(
    [ValidateRange(3, 30)][int]$Rounds = 6,
    [ValidateRange(8, 128)][int]$Objects = 24,
    [ValidateRange(1, 64)][int]$ObjectMiB = 2,
    [string]$SourceBranch = 'dev/stage2a1-autonomous-residency',
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-AutoValidation"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) {
    Write-Host "`n=== $Text ===" -ForegroundColor Cyan
}

function Is-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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
    } catch {
        return $false
    }
}

function Relaunch-Admin {
    $args = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
        '-Rounds', $Rounds,
        '-Objects', $Objects,
        '-ObjectMiB', $ObjectMiB,
        '-SourceBranch', ('"' + $SourceBranch + '"'),
        '-RepoUrl', ('"' + $RepoUrl + '"'),
        '-WorkRoot', ('"' + $WorkRoot + '"')
    )
    Write-Host 'System prerequisites are missing. One UAC prompt is required.' -ForegroundColor Yellow
    $proc = Start-Process powershell.exe -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $proc.ExitCode
}

function Ensure-Prereqs {
    if ($env:OS -ne 'Windows_NT') { throw 'Windows is required.' }

    $needGit = -not (Get-Command git.exe -ErrorAction SilentlyContinue)
    $needVs = -not (Vs-Path)
    $needGraphics = -not (Has-GraphicsTools)

    if (($needGit -or $needVs -or $needGraphics) -and -not (Is-Admin)) {
        Relaunch-Admin
    }

    if ($needGit) {
        Step 'Installing Git for Windows'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'Git is missing and winget is unavailable.' }
        & winget.exe install --id Git.Git --exact --silent --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { throw ('Git installation failed: {0}' -f $LASTEXITCODE) }
        $env:Path = 'C:\Program Files\Git\cmd;C:\Program Files\Git\bin;' + $env:Path
    }

    if (-not (Vs-Path)) {
        Step 'Installing Visual Studio C++ Build Tools'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'MSVC Build Tools are missing and winget is unavailable.' }
        & winget.exe install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-source-agreements --accept-package-agreements --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        if ($LASTEXITCODE -ne 0) { throw ('Build Tools installation failed: {0}' -f $LASTEXITCODE) }
    }

    if (-not (Has-GraphicsTools)) {
        Step 'Installing Windows Graphics Tools'
        $result = Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0'
        if ($result.RestartNeeded) {
            throw 'Graphics Tools were installed, but Windows requests a reboot. Reboot and run the same command again.'
        }
        if (-not (Has-GraphicsTools)) { throw 'Graphics Tools installation failed.' }
    }

    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is unavailable.' }
    if (-not (Vs-Path)) { throw 'MSVC Build Tools are unavailable.' }
}

function Machine-Info {
    $gpu = @()
    try {
        $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
                adapter_ram = [uint64]$_.AdapterRAM
            }
        })
    } catch {}

    $cpu = $null
    try {
        $c = Get-CimInstance Win32_Processor | Select-Object -First 1
        if ($c) {
            $cpu = [ordered]@{
                name = $c.Name
                cores = [int]$c.NumberOfCores
                logical_processors = [int]$c.NumberOfLogicalProcessors
            }
        }
    } catch {}

    $os = $null
    $ram = 0
    try {
        $o = Get-CimInstance Win32_OperatingSystem
        if ($o) {
            $os = [ordered]@{
                caption = $o.Caption
                version = $o.Version
                build = $o.BuildNumber
            }
            $ram = [uint64]$o.TotalVisibleMemorySize * 1024
        }
    } catch {}

    return [ordered]@{
        computer = $env:COMPUTERNAME
        user = $env:USERNAME
        powershell = $PSVersionTable.PSVersion.ToString()
        cpu = $cpu
        ram_bytes = $ram
        gpus = $gpu
        os = $os
    }
}

Ensure-Prereqs

$repoDir = Join-Path $WorkRoot 'repo'
$reportsRoot = Join-Path $WorkRoot 'reports'
New-Item -ItemType Directory -Force -Path $WorkRoot, $reportsRoot | Out-Null

Step 'Preparing clean ARC checkout'
if (-not (Test-Path (Join-Path $repoDir '.git'))) {
    if (Test-Path $repoDir) { Remove-Item -Recurse -Force $repoDir }
    & git.exe clone $RepoUrl $repoDir
    if ($LASTEXITCODE -ne 0) { throw ('git clone failed: {0}' -f $LASTEXITCODE) }
}

$finalExit = 1
Push-Location $repoDir
try {
    & git.exe remote set-url origin $RepoUrl
    & git.exe fetch origin $SourceBranch --prune
    if ($LASTEXITCODE -ne 0) { throw ('git fetch failed: {0}' -f $LASTEXITCODE) }

    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe checkout --detach ('origin/' + $SourceBranch)
    if ($LASTEXITCODE -ne 0) { throw ('git checkout failed: {0}' -f $LASTEXITCODE) }

    $sourceSha = (& git.exe rev-parse HEAD).Trim()
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $safeHost = (($env:COMPUTERNAME -replace '[^A-Za-z0-9._-]', '-') -replace '-+', '-')
    if (-not $safeHost) { $safeHost = 'windows-host' }
    $runId = $stamp + '-' + $safeHost
    $localReportDir = Join-Path $reportsRoot $runId
    New-Item -ItemType Directory -Force -Path $localReportDir | Out-Null
    $logPath = Join-Path $localReportDir 'validation.log'

    $machine = Machine-Info
    $machine | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $localReportDir 'machine.json') -Encoding UTF8

    Step ('Running Stage 2A.1 validation on ' + $sourceSha)
    $validationExit = 0
    $validationError = $null
    try {
        & (Join-Path $repoDir 'scripts\stage2a1-validate.ps1') -Rounds $Rounds -Objects $Objects -ObjectMiB $ObjectMiB *>&1 | Tee-Object -FilePath $logPath -Append
        $validationExit = $LASTEXITCODE
        if ($validationExit -ne 0) { throw ('stage2a1-validate.ps1 returned {0}' -f $validationExit) }
    } catch {
        $validationError = $_.Exception.Message
        if ($validationExit -eq 0) { $validationExit = 1 }
        ('VALIDATION ERROR: ' + $validationError) | Tee-Object -FilePath $logPath -Append | Write-Host
    }

    Step 'Collecting report artifacts'
    $calibrate = Join-Path $repoDir 'build\Release\arc-calibrate.exe'
    if (Test-Path $calibrate) {
        $calLog = Join-Path $localReportDir 'calibration.log'
        try {
            $profile = Join-Path $localReportDir 'hardware-profile.json'
            $scratch = Join-Path $localReportDir 'calibration-scratch.bin'
            & $calibrate $profile $scratch *>&1 | Tee-Object -FilePath $calLog -Append
            if (Test-Path $scratch) { Remove-Item -Force $scratch }
        } catch {
            ('Calibration failed: ' + $_.Exception.Message) | Set-Content -LiteralPath $calLog -Encoding UTF8
        }
    }

    $traces = Join-Path $repoDir 'traces'
    if (Test-Path $traces) {
        Get-ChildItem -LiteralPath $traces -File | Where-Object {
            $_.Extension -in @('.json', '.csv', '.txt') -or $_.Name -like '*.session.json'
        } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $localReportDir -Force
        }
    }

    $summaryPath = Join-Path $localReportDir 'residency-benchmark-summary.json'
    $summary = $null
    if (Test-Path $summaryPath) {
        try { $summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json } catch {}
    }

    $status = if ($validationExit -eq 0) { 'PASS' } else { 'FAIL' }
    $manifest = [ordered]@{
        schema = 1
        status = $status
        validation_exit_code = $validationExit
        validation_error = $validationError
        source_branch = $SourceBranch
        source_sha = $sourceSha
        run_id = $runId
        timestamp_local = (Get-Date).ToString('o')
        rounds = $Rounds
        objects = $Objects
        object_mib = $ObjectMiB
        machine = $machine
        benchmark_summary = $summary
    }
    $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $localReportDir 'manifest.json') -Encoding UTF8

    $report = New-Object System.Collections.Generic.List[string]
    $report.Add('# ARC Stage 2A.1 validation report')
    $report.Add('')
    $report.Add(('- Status: **{0}**' -f $status))
    $report.Add(('- Validation exit code: {0}' -f $validationExit))
    $report.Add(('- Source branch: {0}' -f $SourceBranch))
    $report.Add(('- Source SHA: {0}' -f $sourceSha))
    $report.Add(('- Run ID: {0}' -f $runId))
    $report.Add(('- Machine: {0}' -f $machine.computer))
    if ($machine.cpu) { $report.Add(('- CPU: {0}' -f $machine.cpu.name)) }
    if ($machine.gpus.Count -gt 0) { $report.Add(('- GPU: {0} / driver {1}' -f $machine.gpus[0].name, $machine.gpus[0].driver_version)) }
    if ($validationError) { $report.Add(('- Error: {0}' -f $validationError)) }
    $report.Add('')
    $report.Add('## Included artifacts')
    $report.Add('')
    $report.Add('- manifest.json')
    $report.Add('- machine.json')
    $report.Add('- validation.log')
    if (Test-Path (Join-Path $localReportDir 'hardware-profile.json')) { $report.Add('- hardware-profile.json') }
    if (Test-Path $summaryPath) { $report.Add('- residency-benchmark-summary.json') }
    if (Test-Path (Join-Path $localReportDir 'residency-benchmark.json')) { $report.Add('- residency-benchmark.json') }
    $report.Add('')
    $report.Add('The commit is based directly on the tested source SHA. Raw arcbin traces and build outputs are excluded to avoid repository bloat.')
    $report | Set-Content -LiteralPath (Join-Path $localReportDir 'REPORT.md') -Encoding UTF8

    Step 'Creating report commit'
    $repoReportDir = Join-Path $repoDir ('validation-results\' + $runId)
    New-Item -ItemType Directory -Force -Path $repoReportDir | Out-Null
    Copy-Item -Path (Join-Path $localReportDir '*') -Destination $repoReportDir -Recurse -Force

    $reportBranch = 'results/stage2a1-' + $safeHost + '-' + $stamp
    & git.exe switch -c $reportBranch
    if ($LASTEXITCODE -ne 0) { throw ('git switch failed: {0}' -f $LASTEXITCODE) }

    $name = & git.exe config user.name 2>$null
    if (-not $name) { & git.exe config user.name 'ARC Validation' }
    $email = & git.exe config user.email 2>$null
    if (-not $email) { & git.exe config user.email 'arc-validation@users.noreply.github.com' }

    & git.exe add -- ('validation-results/' + $runId)
    & git.exe commit -m ('Add Stage 2A.1 validation report {0} [{1}]' -f $runId, $status)
    if ($LASTEXITCODE -ne 0) { throw ('git commit failed: {0}' -f $LASTEXITCODE) }
    $reportCommit = (& git.exe rev-parse HEAD).Trim()

    Step ('Uploading report to ' + $reportBranch)
    & git.exe push -u origin ('HEAD:refs/heads/' + $reportBranch)
    $pushOk = $LASTEXITCODE -eq 0

    Write-Host ''
    Write-Host '==============================================' -ForegroundColor Green
    Write-Host ('ARC validation status: ' + $status) -ForegroundColor $(if ($status -eq 'PASS') { 'Green' } else { 'Red' })
    Write-Host ('Source SHA: ' + $sourceSha)
    Write-Host ('Local report: ' + $localReportDir)
    Write-Host ('Report commit: ' + $reportCommit)
    Write-Host ('Report branch: ' + $reportBranch)
    if ($pushOk) {
        Write-Host ('GitHub: https://github.com/Flyingzaptop/ARC/tree/' + $reportBranch) -ForegroundColor Green
    } else {
        Write-Host 'GitHub upload failed. The complete local report was preserved.' -ForegroundColor Red
    }
    Write-Host '=============================================='

    if (-not $pushOk) {
        $finalExit = 3
    } else {
        $finalExit = $validationExit
    }
}
finally {
    Pop-Location
}

exit $finalExit
