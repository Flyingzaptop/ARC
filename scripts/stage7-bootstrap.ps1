param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage7-mixed-graphics-benchmark',
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

if ($env:OS -ne 'Windows_NT') { throw 'Stage 7 mixed graphics benchmark requires Windows.' }
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

    Step 'Building Stage 7 Mixed Graphics package'
    $env:VSLANG = '1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build build --config Release --target `
        arc-adaptive-quality-tests `
        arc-action-effect-tracker-tests `
        arc-adaptive-quality-controller-tests `
        arc-quality-profile-tests `
        arc-render-quality-model-tests `
        dx12-mixed-graphics-benchmark `
        arc-stage7-benchmark-ui
    if ($LASTEXITCODE -ne 0) { throw "Stage 7 build failed: $LASTEXITCODE" }

    Step 'Running Stage 6 + Stage 7 policy tests'
    & $ctest --test-dir build -C Release -R 'arc-(adaptive-quality|action-effect-tracker|quality-profile|render-quality-model)' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Adaptive/Render Quality tests failed: $LASTEXITCODE" }

    Step 'Running 6-second GPU smoke'
    $smoke = Join-Path $repo 'results\stage7-smoke.json'
    if (Test-Path -LiteralPath $smoke) { Remove-Item -LiteralPath $smoke -Force }
    $exe = Join-Path $repo 'build\Release\dx12-mixed-graphics-benchmark.exe'
    $oldEap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $exe --seconds 6 --output $smoke
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldEap }
    if ($code -ne 0) { throw "Stage 7 GPU smoke failed with exit code $code" }
    if (-not (Test-Path -LiteralPath $smoke)) { throw 'Stage 7 GPU smoke produced no JSON.' }
    $smokeJson = Get-Content -LiteralPath $smoke -Raw | ConvertFrom-Json
    if (-not [bool]$smokeJson.valid) { throw 'Stage 7 GPU smoke JSON is invalid.' }
    if ([bool]$smokeJson.temporal_used) { throw 'Stage 7 GPU smoke unexpectedly used temporal assistance.' }
    if (@($smokeJson.actions).Count -eq 0) { throw 'Stage 7 GPU smoke selected no native quality actions.' }
    Write-Host "GPU smoke PASS: $([math]::Round([double]$smokeJson.baseline.p50_ms,3)) -> $([math]::Round([double]$smokeJson.adaptive.p50_ms,3)) ms; actions=$(@($smokeJson.actions).Count)" -ForegroundColor Green

    Step 'Launching persistent Stage 7 UI'
    $ui = Join-Path $repo 'build\Release\arc-stage7-benchmark-ui.exe'
    $process = Start-Process -FilePath $ui -WorkingDirectory $repo -PassThru
    Start-Sleep -Milliseconds 750
    $process.Refresh()
    if ($process.HasExited) { throw "Stage 7 benchmark UI exited immediately with code $($process.ExitCode)." }

    Write-Host 'ARC Stage 7 Mixed Graphics Benchmark opened.' -ForegroundColor Green
    Write-Host 'Click Run 60s Mixed Benchmark. PowerShell will stay open after completion.' -ForegroundColor Yellow
    Write-Host 'PASS/FAIL, metrics and Results URL will also remain visible in the ARC window.' -ForegroundColor Green
} finally {
    Pop-Location
}
