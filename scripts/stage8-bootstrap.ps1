param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage8-closed-loop-quality',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage8",
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

if ($env:OS -ne 'Windows_NT') { throw 'Stage 8 benchmark requires Windows.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is required.' }

$repo = Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null
Step 'Preparing ARC Stage 8 checkout'
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
    if ($ExpectedSha -and $sha -ne $ExpectedSha) { throw "Stage 8 SHA mismatch. Expected $ExpectedSha, got $sha" }

    $cmake = Ensure-CMake
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest.exe not found next to cmake.exe.' }
    Write-Host "Using CMake: $cmake"

    Step 'Building closed-loop Adaptive Quality Core'
    $env:VSLANG = '1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target `
        arc-adaptive-quality-tests `
        arc-action-effect-tracker-tests `
        arc-adaptive-quality-controller-tests `
        arc-quality-profile-tests `
        arc-closed-loop-quality-sim-tests `
        arc-quality-restore-memory-pressure-tests `
        dx12-mixed-graphics-benchmark `
        dx12-closed-loop-mixed-graphics-benchmark `
        arc-stage8-benchmark-ui
    if ($LASTEXITCODE -ne 0) { throw "Stage 8 build failed: $LASTEXITCODE" }

    Step 'Running Adaptive Quality + closed-loop CPU tests'
    & $ctest --test-dir build -C Release -R 'arc-(adaptive-quality|action-effect-tracker|quality-profile|closed-loop-quality|quality-restore-memory-pressure)' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Stage 8 CPU tests failed: $LASTEXITCODE" }

    Step 'Running short Stage 8 closed-loop GPU smoke gate'
    $exe = Join-Path $repo 'build\Release\dx12-closed-loop-mixed-graphics-benchmark.exe'
    $smoke = Join-Path $repo 'traces\stage8-bootstrap-smoke.json'
    New-Item -ItemType Directory -Force (Split-Path -Parent $smoke) | Out-Null
    Remove-Item -LiteralPath $smoke -Force -ErrorAction SilentlyContinue
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $exe --seconds 12 --probe-frames 4 --control-frames 8 --output $smoke 2>&1 | ForEach-Object { Write-Host $_ }
        $smokeExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previous }
    if($smokeExit -ne 0){ throw "Stage 8 closed-loop GPU smoke failed: $smokeExit" }
    if(-not (Test-Path -LiteralPath $smoke)){ throw 'Stage 8 smoke produced no JSON.' }
    $j=Get-Content -LiteralPath $smoke -Raw | ConvertFrom-Json
    if(-not [bool]$j.valid){ throw 'Stage 8 smoke JSON invalid.' }
    if([string]$j.benchmark -ne 'closed_loop_mixed_graphics'){ throw 'Wrong Stage 8 benchmark schema.' }
    if([bool]$j.temporal_used){ throw 'Temporal assistance unexpectedly used.' }
    if(@($j.scene_calibration).Count -lt 5){ throw 'Stage 8 smoke did not calibrate all scene domains.' }
    if(@($j.quality_probes).Count -lt 15){ throw 'Stage 8 smoke did not measure all quality ladder steps.' }
    if([int]$j.controller.degrade_events -lt 1){ throw 'Stage 8 smoke made no live degrade decision.' }
    Write-Host "Stage 8 GPU smoke PASS: baseline misses $([math]::Round(100*[double]$j.baseline.miss_ratio,1))% -> ARC $([math]::Round(100*[double]$j.adaptive.miss_ratio,1))%" -ForegroundColor Green

    Step 'Launching Stage 8 Benchmark Center'
    $ui = Join-Path $repo 'build\Release\arc-stage8-benchmark-ui.exe'
    $process = Start-Process -FilePath $ui -WorkingDirectory $repo -PassThru
    Start-Sleep -Milliseconds 800
    $process.Refresh()
    if($process.HasExited){ throw "Stage 8 benchmark UI exited immediately with code $($process.ExitCode)." }

    Write-Host 'ARC Stage 8 Benchmark Center opened.' -ForegroundColor Green
    Write-Host 'CPU policy tests and the short closed-loop GPU smoke gate passed.' -ForegroundColor Green
    Write-Host 'Click Run 60s Closed-Loop Benchmark.' -ForegroundColor Yellow
    Write-Host 'The UI will retain PASS/FAIL, budget-miss reduction, recovery state and Results URL.' -ForegroundColor Green
} finally {
    Pop-Location
}
