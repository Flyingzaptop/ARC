param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage7-mixed-graphics',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage7",
    [string]$ExpectedSha = ''
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
    if (-not $cmake) { throw 'CMake installed but could not be located.' }
    return $cmake
}

if ($env:OS -ne 'Windows_NT') { throw 'Stage 7 benchmark requires Windows.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is required.' }

$repo = Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null
Step 'Preparing ARC Stage 7 checkout'
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
    if ($ExpectedSha -and $sha -ne $ExpectedSha) { throw "Stage 7 SHA mismatch. Expected $ExpectedSha, got $sha" }

    $cmake = Ensure-CMake
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest.exe not found next to cmake.exe.' }
    Write-Host "Using CMake: $cmake"

    Step 'Building Adaptive Quality Core + mixed graphics benchmark'
    $env:VSLANG = '1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target `
        arc-adaptive-quality-tests `
        arc-action-effect-tracker-tests `
        arc-adaptive-quality-controller-tests `
        arc-quality-profile-tests `
        dx12-adaptive-quality-benchmark `
        dx12-mixed-graphics-benchmark `
        arc-stage7-benchmark-ui
    if ($LASTEXITCODE -ne 0) { throw "Stage 7 build failed: $LASTEXITCODE" }

    Step 'Running Adaptive Quality policy tests'
    & $ctest --test-dir build -C Release -R 'arc-(adaptive-quality|action-effect-tracker|quality-profile)' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Adaptive Quality tests failed: $LASTEXITCODE" }

    Step 'Running short Stage 6 compute sanity check'
    $stage6Exe = Join-Path $repo 'build\Release\dx12-adaptive-quality-benchmark.exe'
    $stage6Smoke = Join-Path $repo 'traces\stage7-stage6-sanity.json'
    New-Item -ItemType Directory -Force (Split-Path -Parent $stage6Smoke) | Out-Null
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $stage6Exe --seconds 6 --headless --output $stage6Smoke 2>&1 | ForEach-Object { Write-Host $_ }
        $stage6Exit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previous }
    if($stage6Exit -ne 0){ throw "Stage 6 sanity check failed: $stage6Exit" }

    Step 'Running 8-second mixed graphics GPU smoke gate'
    $mixedExe = Join-Path $repo 'build\Release\dx12-mixed-graphics-benchmark.exe'
    $smoke = Join-Path $repo 'traces\stage7-bootstrap-smoke.json'
    Remove-Item -LiteralPath $smoke -Force -ErrorAction SilentlyContinue
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $mixedExe --seconds 8 --probe-frames 4 --output $smoke 2>&1 | ForEach-Object { Write-Host $_ }
        $smokeExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previous }
    if($smokeExit -ne 0){ throw "Stage 7 mixed graphics smoke failed: $smokeExit" }
    if(-not (Test-Path -LiteralPath $smoke)){ throw 'Mixed graphics smoke produced no JSON.' }
    $j=Get-Content -LiteralPath $smoke -Raw | ConvertFrom-Json
    if(-not [bool]$j.valid){ throw 'Mixed graphics smoke JSON invalid.' }
    if([string]$j.benchmark -ne 'mixed_graphics'){ throw 'Wrong Stage 7 benchmark schema.' }
    if([bool]$j.temporal_used){ throw 'Temporal assistance unexpectedly used.' }
    if(@($j.probes).Count -lt 5){ throw 'Mixed graphics smoke did not probe all quality domains.' }
    if(@($j.actions).Count -eq 0){ throw 'Mixed graphics smoke selected no adaptive actions.' }
    Write-Host "Mixed GPU smoke PASS: baseline P50 $([math]::Round([double]$j.baseline.p50_ms,3)) ms -> adaptive P50 $([math]::Round([double]$j.adaptive.p50_ms,3)) ms" -ForegroundColor Green

    Step 'Launching Stage 7 Benchmark Center'
    $ui = Join-Path $repo 'build\Release\arc-stage7-benchmark-ui.exe'
    $process = Start-Process -FilePath $ui -WorkingDirectory $repo -PassThru
    Start-Sleep -Milliseconds 800
    $process.Refresh()
    if($process.HasExited){ throw "Stage 7 benchmark UI exited immediately with code $($process.ExitCode)." }

    Write-Host 'ARC Stage 7 Benchmark Center opened.' -ForegroundColor Green
    Write-Host 'Both GPU smoke gates passed. Click Run 60s Mixed Benchmark.' -ForegroundColor Yellow
    Write-Host 'The UI will retain PASS/FAIL, P50/P99, actions and Results URL.' -ForegroundColor Green
    Write-Host 'PowerShell will wait for Enter after the full run instead of disappearing.' -ForegroundColor Green
} finally {
    Pop-Location
}
