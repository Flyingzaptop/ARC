param(
    [string]$SourceRef = 'dev/stage5-steam-launcher',
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Launcher"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1)
        if ($vs) {
            $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw 'cmake.exe was not found. Install Visual Studio C++ Build Tools with the CMake component.'
}

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe was not found.' }
$cmake = Find-CMake
$repo = Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null

Step 'Preparing ARC Stage 5 launcher checkout'
if (-not (Test-Path (Join-Path $repo '.git'))) {
    if (Test-Path $repo) { Remove-Item -Recurse -Force $repo }
    & git.exe clone $RepoUrl $repo
    if ($LASTEXITCODE -ne 0) { throw "git clone failed: $LASTEXITCODE" }
}
Push-Location $repo
try {
    & git.exe remote set-url origin $RepoUrl
    & git.exe fetch origin $SourceRef --prune
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed: $LASTEXITCODE" }
    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe switch -C dev/stage5-steam-launcher FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "git switch failed: $LASTEXITCODE" }
    $sourceSha = (& git.exe rev-parse HEAD).Trim()
    Write-Host "Source SHA: $sourceSha"

    Step 'Configuring and building ARC Launcher'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target arc-launcher arc-game-monitor arc-steam-library-tests
    if ($LASTEXITCODE -ne 0) { throw "Launcher build failed: $LASTEXITCODE" }
    & ctest.exe --test-dir build -C Release -R arc-steam-library-tests --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Steam scanner tests failed: $LASTEXITCODE" }

    Step 'Preparing official PresentMon 2.5.1'
    $releaseDir = Join-Path $repo 'build\Release'
    $presentMon = Join-Path $releaseDir 'PresentMon.exe'
    $expectedSha256 = '9bec3083069f58f911e6a512f4806db51a27bd096103087bc1d05ef54c80a191'
    $needsDownload = $true
    if (Test-Path $presentMon) {
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $presentMon).Hash.ToLowerInvariant()
        $needsDownload = $actual -ne $expectedSha256
    }
    if ($needsDownload) {
        $url = 'https://github.com/GameTechDev/PresentMon/releases/download/v2.5.1/PresentMon-2.5.1-x64.exe'
        Invoke-WebRequest $url -OutFile $presentMon
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $presentMon).Hash.ToLowerInvariant()
        if ($actual -ne $expectedSha256) { throw "PresentMon SHA256 mismatch: $actual" }
    }

    Step 'Launching ARC'
    $launcher = Join-Path $releaseDir 'arc-launcher.exe'
    if (-not (Test-Path $launcher)) { throw 'arc-launcher.exe is missing after build.' }
    Start-Process -FilePath $launcher -WorkingDirectory $releaseDir
    Write-Host ''
    Write-Host 'ARC Launcher started.' -ForegroundColor Green
    Write-Host 'Reports: Documents\ARC\Sessions'
} finally {
    Pop-Location
}
