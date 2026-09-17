param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage5-steam-launcher-final2',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage5",
    [switch]$SkipPresentMonInstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

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

    Step 'Building ARC Launcher'
    $env:VSLANG = '1033'
    & cmake.exe -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake.exe --build build --config Release --target arc-launcher arc-game-monitor arc-steam-library-tests
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    & ctest.exe --test-dir build -C Release -R arc-steam-library-tests --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Steam scanner test failed: $LASTEXITCODE" }

    Step 'Launching ARC'
    $launcher = Join-Path $repo 'build\Release\arc-launcher.exe'
    Start-Process -FilePath $launcher -WorkingDirectory $repo | Out-Null
    Write-Host "ARC Launcher opened. Captures: $env:LOCALAPPDATA\ARC\captures" -ForegroundColor Green
} finally {
    Pop-Location
}
