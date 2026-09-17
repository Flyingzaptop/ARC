param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage6-adaptive-quality-core',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage6"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

function Find-CMake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($installation in @(& $vswhere -products * -property installationPath)) {
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
        foreach ($root in @($env:ProgramFiles, $env:LOCALAPPDATA)) {
            if (-not $root) { continue }
            $match = Get-ChildItem -Path $root -Filter cmake.exe -File -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '[\\/]CMake[\\/]bin[\\/]cmake\.exe$' } |
                Select-Object -First 1
            if ($match) { return $match.FullName }
        }
        throw 'CMake installed but could not be located.'
    }
    return $cmake
}

if ($env:OS -ne 'Windows_NT') { throw 'Stage 6 benchmark requires Windows.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is required.' }

$repo = Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null
Step 'Preparing ARC Stage 6 checkout'
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

    $cmake = Ensure-CMake
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest.exe not found next to cmake.exe.' }
    Write-Host "Using CMake: $cmake"

    Step 'Building Adaptive Quality Core + benchmark'
    $env:VSLANG = '1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target arc-adaptive-quality-tests dx12-adaptive-quality-benchmark arc-stage6-benchmark-ui
    if ($LASTEXITCODE -ne 0) { throw "Stage 6 build failed: $LASTEXITCODE" }

    Step 'Running CPU policy tests'
    & $ctest --test-dir build -C Release -R arc-adaptive-quality-tests --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Adaptive Quality tests failed: $LASTEXITCODE" }

    Step 'Launching Stage 6 benchmark UI'
    $ui = Join-Path $repo 'build\Release\arc-stage6-benchmark-ui.exe'
    Start-Process -FilePath $ui -WorkingDirectory $repo | Out-Null
    Write-Host 'ARC Stage 6 Benchmark opened.' -ForegroundColor Green
    Write-Host 'Close games / GPU-heavy apps, then click Run 60s Benchmark.' -ForegroundColor Yellow
    Write-Host 'The benchmark publishes a results/stage6-* branch automatically.' -ForegroundColor Green
} finally {
    Pop-Location
}
