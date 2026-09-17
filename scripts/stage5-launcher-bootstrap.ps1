param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage5-steam-launcher-final2',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage5",
    [switch]$SkipPresentMonInstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installations = @(& $vswhere -products * -property installationPath)
        foreach ($installation in $installations) {
            if (-not $installation) { continue }
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    return $null
}

function Ensure-CMake {
    $cmake = Find-CMake
    if ($cmake) { return $cmake }

    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) { throw 'CMake is not installed and winget is unavailable.' }

    Step 'Installing CMake'
    & $winget.Source install --id Kitware.CMake -e --silent --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "CMake installation failed: $LASTEXITCODE" }

    $cmake = Find-CMake
    if (-not $cmake) {
        # winget can update PATH only for future shells. Probe common package locations again explicitly.
        foreach ($root in @($env:ProgramFiles, $env:LOCALAPPDATA)) {
            if (-not $root) { continue }
            $match = Get-ChildItem -Path $root -Filter cmake.exe -File -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '[\\/]CMake[\\/]bin[\\/]cmake\.exe$' } |
                Select-Object -First 1
            if ($match) { $cmake = $match.FullName; break }
        }
    }
    if (-not $cmake) { throw 'CMake installed but cmake.exe could not be located. Open a new shell and rerun.' }
    return $cmake
}

if ($env:OS -ne 'Windows_NT') { throw 'Stage 5 launcher requires Windows.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is required.' }

$repo = Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null
Step 'Preparing ARC Stage 5 checkout'
if (-not (Test-Path (Join-Path $repo '.git'))) {
    & git.exe clone $RepoUrl $repo
    if ($LASTEXITCODE -ne 0) { throw "git clone failed: $LASTEXITCODE" }
}
Push-Location $repo
try {
    & git.exe fetch origin $Branch --prune
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed: $LASTEXITCODE" }
    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe switch -C $Branch ("origin/" + $Branch)
    if ($LASTEXITCODE -ne 0) { throw "git switch failed: $LASTEXITCODE" }
    $sha = (& git.exe rev-parse HEAD).Trim()
    Write-Host "Source SHA: $sha"

    if (-not $SkipPresentMonInstall) {
        $presentMon = Get-Command PresentMon.exe -ErrorAction SilentlyContinue
        $wingetLink = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\PresentMon.exe'
        if (-not $presentMon -and -not (Test-Path $wingetLink)) {
            if (Get-Command winget.exe -ErrorAction SilentlyContinue) {
                Step 'Installing PresentMon Console'
                & winget.exe install --id Intel.PresentMon.Console -e --silent --accept-source-agreements --accept-package-agreements
                if ($LASTEXITCODE -ne 0) { Write-Host "PresentMon install returned $LASTEXITCODE; launcher will still open." -ForegroundColor Yellow }
            } else {
                Write-Host 'winget unavailable; PresentMon can be installed later from Advanced.' -ForegroundColor Yellow
            }
        }
    }

    $cmake = Ensure-CMake
    $cmakeDir = Split-Path -Parent $cmake
    $ctest = Join-Path $cmakeDir 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) { throw "ctest.exe not found next to CMake: $cmakeDir" }
    Write-Host "Using CMake: $cmake"

    Step 'Building ARC Launcher'
    $env:VSLANG = '1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target arc-launcher arc-game-monitor arc-steam-library-tests
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    & $ctest --test-dir build -C Release -R arc-steam-library-tests --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Steam scanner test failed: $LASTEXITCODE" }

    Step 'Launching ARC'
    $launcher = Join-Path $repo 'build\Release\arc-launcher.exe'
    Start-Process -FilePath $launcher -WorkingDirectory $repo | Out-Null
    Write-Host "ARC Launcher opened. Captures: $env:LOCALAPPDATA\ARC\captures" -ForegroundColor Green
} finally {
    Pop-Location
}
