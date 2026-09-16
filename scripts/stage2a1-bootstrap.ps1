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
$script:ValidationExitCode = 0
$script:PushSucceeded = $false
$script:ReportBranch = $null
$script:ReportCommit = $null
$script:SourceSha = $null
$script:RunId = $null
$script:RunDirectory = $null
$script:LogPath = $null

function Write-Step([string]$Message) {
    Write-Host "`n=== $Message ===" -ForegroundColor Cyan
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-VsInstallation {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $install) { return $null }
    return ($install | Select-Object -First 1)
}

function Test-GraphicsToolsInstalled {
    try {
        $cap = Get-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' -ErrorAction Stop
        return $cap.State -eq 'Installed'
    } catch {
        return $false
    }
}

function Relaunch-Elevated {
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
        '-Rounds', $Rounds,
        '-Objects', $Objects,
        '-ObjectMiB', $ObjectMiB,
        '-SourceBranch', ('"' + $SourceBranch + '"'),
        '-RepoUrl', ('"' + $RepoUrl + '"'),
        '-WorkRoot', ('"' + $WorkRoot + '"')
    )
    Write-Host 'Missing system prerequisites. Requesting one UAC elevation to install them...' -ForegroundColor Yellow
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

function Ensure-Prerequisites {
    if ($env:OS -ne 'Windows_NT') { throw 'This bootstrap is for Windows only.' }

    $gitMissing = -not (Get-Command git.exe -ErrorAction SilentlyContinue)
    $vsMissing = -not (Get-VsInstallation)
    $graphicsMissing = -not (Test-GraphicsToolsInstalled)

    if (($gitMissing -or $vsMissing -or $graphicsMissing) -and -not (Test-Administrator)) {
        Relaunch-Elevated
    }

    if ($gitMissing) {
        Write-Step 'Installing Git for Windows'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'Git is missing and winget is unavailable.' }
        & winget.exe install --id Git.Git --exact --silent --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { throw "Git installation failed with exit code $LASTEXITCODE" }
        $env:Path = "C:\Program Files\Git\cmd;C:\Program Files\Git\bin;$env:Path"
    }

    if (-not (Get-VsInstallation)) {
        Write-Step 'Installing Visual Studio Build Tools + C++ workload'
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'MSVC Build Tools are missing and winget is unavailable.' }
        & winget.exe install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-source-agreements --accept-package-agreements --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        if ($LASTEXITCODE -ne 0) { throw "Visual Studio Build Tools installation failed with exit code $LASTEXITCODE" }
    }

    if (-not (Test-GraphicsToolsInstalled)) {
        Write-Step 'Installing Windows Graphics Tools / D3D12 debug layer'
        $result = Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0'
        if ($result.RestartNeeded) {
            throw 'Graphics Tools installed but Windows requests a reboot. Reboot once, then run the same one-line command again.'
        }
        if (-not (Test-GraphicsToolsInstalled)) { throw 'Graphics Tools installation did not complete successfully.' }
    }

    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is still unavailable after prerequisite setup.' }
    if (-not (Get-VsInstallation)) { throw 'MSVC Build Tools are still unavailable after prerequisite setup.' }
}

function Invoke-Logged {
    param(
        [Parameter(Mandatory=$true)][scriptblock]$Script,
        [Parameter(Mandatory=$true)][string]$LogFile
    )
    & $Script *>&1 | Tee-Object -FilePath $LogFile -Append
    return $LASTEXITCODE
}

function Get-MachineSnapshot {
    $gpus = @()
    try {
        $gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
                adapter_ram = [uint64]$_.AdapterRAM
            }
        })
    } catch {}

    $cpu = $null
    try {
        $cpuObject = Get-CimInstance Win32_Processor | Select-Object -First 1
        if ($cpuObject) {
            $cpu = [ordered]@{
                name = $cpuObject.Name
                cores = [int]$cpuObject.NumberOfCores
                logical_processors = [int]$cpuObject.NumberOfLogicalProcessors
            }
        }
    } catch {}

    $os = $null
    $ram = $null
    try {
        $osObject = Get-CimInstance Win32_OperatingSystem
        if ($osObject) {
            $os = [ordered]@{
                caption = $osObject.Caption
                version = $osObject.Version
                build_number = $osObject.BuildNumber
            }
            $ram = [uint64]$osObject.TotalVisibleMemorySize * 1024
        }
    } catch {}

    return [ordered]@{
        computer_name = $env:COMPUTERNAME
        user_name = $env:USERNAME
        powershell = $PSVersionTable.PSVersion.ToString()
        cpu = $cpu
        ram_bytes = $ram
        gpus = $gpus
        os = $os
    }
}

function Copy-IfExists([string]$Source, [string]$DestinationDirectory) {
    if (Test-Path $Source) {
        Copy-Item -LiteralPath $Source -Destination $DestinationDirectory -Force
    }
}

Ensure-Prerequisites

$repoDirectory = Join-Path $WorkRoot 'repo'
$reportsDirectory = Join-Path $WorkRoot 'reports'
New-Item -ItemType Directory -Force -Path $WorkRoot, $reportsDirectory | Out-Null

Write-Step 'Preparing clean ARC checkout'
if (-not (Test-Path (Join-Path $repoDirectory '.git'))) {
    if (Test-Path $repoDirectory) { Remove-Item -Recurse -Force $repoDirectory }
    & git.exe clone $RepoUrl $repoDirectory
    if ($LASTEXITCODE -ne 0) { throw "git clone failed with exit code $LASTEXITCODE" }
}

Push-Location $repoDirectory
try {
    & git.exe remote set-url origin $RepoUrl
    & git.exe fetch origin $SourceBranch --prune
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed with exit code $LASTEXITCODE" }

    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe checkout --detach "origin/$SourceBranch"
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed with exit code $LASTEXITCODE" }

    $script:SourceSha = (& git.exe rev-parse HEAD).Trim()
    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $safeHost = (($env:COMPUTERNAME -replace '[^A-Za-z0-9._-]', '-') -replace '-+', '-')
    if (-not $safeHost) { $safeHost = 'windows-host' }
    $script:RunId = "$timestamp-$safeHost"
    $script:RunDirectory = Join-Path $reportsDirectory $script:RunId
    New-Item -ItemType Directory -Force -Path $script:RunDirectory | Out-Null
    $script:LogPath = Join-Path $script:RunDirectory 'validation.log'

    $machine = Get-MachineSnapshot
    $machine | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $script:RunDirectory 'machine.json') -Encoding UTF8

    Write-Step "Running Stage 2A.1 validation on $script:SourceSha"
    $validationError = $null
    try {
        & "$repoDirectory\scripts\stage2a1-validate.ps1" -Rounds $Rounds -Objects $Objects -ObjectMiB $ObjectMiB *>&1 | Tee-Object -FilePath $script:LogPath -Append
        $script:ValidationExitCode = $LASTEXITCODE
        if ($script:ValidationExitCode -ne 0) { throw "stage2a1-validate.ps1 returned $script:ValidationExitCode" }
    } catch {
        $validationError = $_.Exception.Message
        if ($script:ValidationExitCode -eq 0) { $script:ValidationExitCode = 1 }
        "VALIDATION ERROR: $validationError" | Tee-Object -FilePath $script:LogPath -Append | Write-Host
    }

    Write-Step 'Collecting hardware and validation artifacts'
    $calibrationLog = Join-Path $script:RunDirectory 'calibration.log'
    $calibrationExe = Join-Path $repoDirectory 'build\Release\arc-calibrate.exe'
    if (Test-Path $calibrationExe) {
        try {
            $profilePath = Join-Path $script:RunDirectory 'hardware-profile.json'
            $calibrationScratch = Join-Path $script:RunDirectory 'calibration-scratch.bin'
            & $calibrationExe $profilePath $calibrationScratch *>&1 | Tee-Object -FilePath $calibrationLog -Append
            if (Test-Path $calibrationScratch) { Remove-Item -Force $calibrationScratch }
        } catch {
            "Calibration failed: $($_.Exception.Message)" | Set-Content -LiteralPath $calibrationLog -Encoding UTF8
        }
    }

    $traceDirectory = Join-Path $repoDirectory 'traces'
    if (Test-Path $traceDirectory) {
        Get-ChildItem -LiteralPath $traceDirectory -File | Where-Object {
            $_.Extension -in @('.json', '.csv', '.txt') -or $_.Name -like '*.session.json'
        } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $script:RunDirectory -Force
        }
    }

    $benchmarkSummaryPath = Join-Path $script:RunDirectory 'residency-benchmark-summary.json'
    $benchmarkSummary = $null
    if (Test-Path $benchmarkSummaryPath) {
        try { $benchmarkSummary = Get-Content -Raw -LiteralPath $benchmarkSummaryPath | ConvertFrom-Json } catch {}
    }

    $status = if ($script:ValidationExitCode -eq 0) { 'PASS' } else { 'FAIL' }
    $manifest = [ordered]@{
        schema = 1
        status = $status
        validation_exit_code = $script:ValidationExitCode
        validation_error = $validationError
        source_branch = $SourceBranch
        source_sha = $script:SourceSha
        run_id = $script:RunId
        timestamp_local = (Get-Date).ToString('o')
        rounds = $Rounds
        objects = $Objects
        object_mib = $ObjectMiB
        machine = $machine
        benchmark_summary = $benchmarkSummary
    }
    $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $script:RunDirectory 'manifest.json') -Encoding UTF8

    $reportLines = New-Object System.Collections.Generic.List[string]
    $reportLines.Add('# ARC Stage 2A.1 validation report')
    $reportLines.Add('')
    $reportLines.Add("- Status: **$status**")
    $reportLines.Add("- Validation exit code: `$script:ValidationExitCode`")
    $reportLines.Add("- Source branch: `$SourceBranch`")
    $reportLines.Add("- Source SHA: `$script:SourceSha`")
    $reportLines.Add("- Run ID: `$script:RunId`")
    $reportLines.Add("- Machine: `$($machine.computer_name)`")
    if ($machine.cpu) { $reportLines.Add("- CPU: `$($machine.cpu.name)`") }
    if ($machine.gpus.Count -gt 0) { $reportLines.Add("- GPU: `$($machine.gpus[0].name)` / driver `$($machine.gpus[0].driver_version)`") }
    if ($validationError) { $reportLines.Add("- Error: `$validationError`") }
    $reportLines.Add('')
    $reportLines.Add('## Artifacts')
    $reportLines.Add('')
    $reportLines.Add('- `manifest.json`')
    $reportLines.Add('- `machine.json`')
    $reportLines.Add('- `validation.log`')
    if (Test-Path (Join-Path $script:RunDirectory 'hardware-profile.json')) { $reportLines.Add('- `hardware-profile.json`') }
    if (Test-Path $benchmarkSummaryPath) { $reportLines.Add('- `residency-benchmark-summary.json`') }
    if (Test-Path (Join-Path $script:RunDirectory 'residency-benchmark.json')) { $reportLines.Add('- `residency-benchmark.json`') }
    $reportLines.Add('')
    $reportLines.Add('The report commit is based directly on the tested source SHA. Build outputs and raw `.arcbin` traces are intentionally not committed to avoid repository bloat.')
    $reportLines | Set-Content -LiteralPath (Join-Path $script:RunDirectory 'REPORT.md') -Encoding UTF8

    Write-Step 'Creating Git report commit'
    $repoReportPath = Join-Path $repoDirectory ("validation-results\" + $script:RunId)
    New-Item -ItemType Directory -Force -Path $repoReportPath | Out-Null
    Copy-Item -Path (Join-Path $script:RunDirectory '*') -Destination $repoReportPath -Recurse -Force

    $script:ReportBranch = "results/stage2a1-$safeHost-$timestamp"
    & git.exe switch -c $script:ReportBranch
    if ($LASTEXITCODE -ne 0) { throw "git switch failed with exit code $LASTEXITCODE" }

    $gitName = (& git.exe config user.name 2>$null)
    if (-not $gitName) { & git.exe config user.name 'ARC Validation' }
    $gitEmail = (& git.exe config user.email 2>$null)
    if (-not $gitEmail) { & git.exe config user.email 'arc-validation@users.noreply.github.com' }

    & git.exe add -- "validation-results/$script:RunId"
    & git.exe commit -m "Add Stage 2A.1 validation report $script:RunId [$status]"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed with exit code $LASTEXITCODE" }
    $script:ReportCommit = (& git.exe rev-parse HEAD).Trim()

    Write-Step "Uploading report to GitHub branch $script:ReportBranch"
    & git.exe push -u origin "HEAD:refs/heads/$script:ReportBranch"
    if ($LASTEXITCODE -eq 0) {
        $script:PushSucceeded = $true
    } else {
        $script:PushSucceeded = $false
        "Git push failed with exit code $LASTEXITCODE. Git Credential Manager may require an interactive GitHub sign-in on first use." | Tee-Object -FilePath $script:LogPath -Append | Write-Host
    }

    Write-Host ''
    Write-Host '==============================================' -ForegroundColor Green
    Write-Host "ARC validation status: $status" -ForegroundColor $(if ($status -eq 'PASS') { 'Green' } else { 'Red' })
    Write-Host "Source SHA: $script:SourceSha"
    Write-Host "Local report: $script:RunDirectory"
    Write-Host "Report commit: $script:ReportCommit"
    Write-Host "Report branch: $script:ReportBranch"
    if ($script:PushSucceeded) {
        $branchUrl = 'https://github.com/Flyingzaptop/ARC/tree/' + $script:ReportBranch
        Write-Host "GitHub: $branchUrl" -ForegroundColor Green
    } else {
        Write-Host 'GitHub upload: FAILED (local report is preserved)' -ForegroundColor Red
    }
    Write-Host '=============================================='

    if (-not $script:PushSucceeded) { exit 3 }
    exit $script:ValidationExitCode
}
finally {
    Pop-Location
}
