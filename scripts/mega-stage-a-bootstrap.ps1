param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage9-runtime-governor-integration',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Mega-A",
    [string]$ExpectedSha = ''
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
function Step([string]$Text){Write-Host "`n=== $Text ===" -ForegroundColor Cyan}

function Find-CMake {
    $cmd=Get-Command cmake.exe -ErrorAction SilentlyContinue
    if($cmd){return $cmd.Source}
    foreach($candidate in @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )){if($candidate -and (Test-Path -LiteralPath $candidate)){return $candidate}}
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(Test-Path -LiteralPath $vswhere){
        foreach($installation in @(& $vswhere -products * -property installationPath)){
            if(-not$installation){continue}
            $candidate=Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if(Test-Path -LiteralPath $candidate){return $candidate}
        }
    }
    return $null
}

function Ensure-CMake {
    $cmake=Find-CMake
    if($cmake){return $cmake}
    $winget=Get-Command winget.exe -ErrorAction SilentlyContinue
    if(-not$winget){throw 'CMake is not installed and winget is unavailable.'}
    Step 'Installing CMake'
    & $winget.Source install --id Kitware.CMake -e --silent --accept-source-agreements --accept-package-agreements
    if($LASTEXITCODE-ne 0){throw "CMake installation failed: $LASTEXITCODE"}
    $cmake=Find-CMake
    if(-not$cmake){throw 'CMake installed but could not be located.'}
    return $cmake
}

if($env:OS-ne 'Windows_NT'){throw 'Mega Stage A acceptance requires Windows.'}
if(-not(Get-Command git.exe -ErrorAction SilentlyContinue)){throw 'git.exe is required.'}
$repo=Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot|Out-Null

Step 'Preparing ARC Mega Stage A checkout'
if(-not(Test-Path (Join-Path $repo '.git'))){
    & git.exe clone $RepoUrl $repo
    if($LASTEXITCODE-ne 0){throw "git clone failed: $LASTEXITCODE"}
}

Push-Location $repo
try{
    & git.exe fetch origin $Branch --prune
    if($LASTEXITCODE-ne 0){throw "git fetch failed: $LASTEXITCODE"}
    & git.exe reset --hard
    & git.exe clean -fdx
    & git.exe switch -C $Branch ("origin/"+$Branch)
    if($LASTEXITCODE-ne 0){throw "git switch failed: $LASTEXITCODE"}
    $sha=(& git.exe rev-parse HEAD).Trim()
    Write-Host "Source SHA: $sha"
    if($ExpectedSha -and $sha-ne $ExpectedSha){throw "Mega Stage A SHA mismatch. Expected $ExpectedSha, got $sha"}

    $cmake=Ensure-CMake
    $ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if(-not(Test-Path -LiteralPath $ctest)){throw 'ctest.exe not found next to cmake.exe.'}
    Write-Host "Using CMake: $cmake"

    Step 'Building Mega Stage A: Stages 9-12'
    $env:VSLANG='1033'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if($LASTEXITCODE-ne 0){throw "CMake configure failed: $LASTEXITCODE"}
    & $cmake --build build --config Release --target `
        arc-adaptive-quality-tests `
        arc-action-effect-tracker-tests `
        arc-adaptive-quality-controller-tests `
        arc-quality-profile-tests `
        arc-quality-restore-memory-pressure-tests `
        arc-quality-admission-tests `
        arc-global-action-arbiter-tests `
        arc-unified-runtime-governor-tests `
        dx12-mega-stage-a-benchmark `
        arc-mega-stage-a-benchmark-ui
    if($LASTEXITCODE-ne 0){throw "Mega Stage A build failed: $LASTEXITCODE"}

    Step 'Running Mega Stage A CPU and policy gates'
    & $ctest --test-dir build -C Release -R 'arc-(adaptive-quality|action-effect-tracker|quality-profile|quality-restore-memory-pressure|quality-admission|global-action-arbiter|unified-runtime-governor)' --output-on-failure
    if($LASTEXITCODE-ne 0){throw "Mega Stage A policy tests failed: $LASTEXITCODE"}

    Step 'Running integrated 30-second GPU smoke gate'
    $exe=Join-Path $repo 'build\Release\dx12-mega-stage-a-benchmark.exe'
    $smoke=Join-Path $repo 'traces\mega-stage-a-bootstrap-smoke.json'
    New-Item -ItemType Directory -Force (Split-Path -Parent $smoke)|Out-Null
    Remove-Item -LiteralPath $smoke -Force -ErrorAction SilentlyContinue
    $previous=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        & $exe --seconds 30 --probe-frames 4 --control-frames 8 --output $smoke 2>&1|ForEach-Object{Write-Host $_}
        $smokeExit=$LASTEXITCODE
    }finally{$ErrorActionPreference=$previous}
    if($smokeExit-ne 0){throw "Mega Stage A GPU smoke failed: $smokeExit"}
    if(-not(Test-Path -LiteralPath $smoke)){throw 'Mega Stage A smoke produced no JSON.'}
    $j=Get-Content -LiteralPath $smoke -Raw|ConvertFrom-Json
    if(-not[bool]$j.valid){throw 'Mega Stage A smoke JSON invalid.'}
    if([string]$j.benchmark-ne 'mega_stage_a'){throw 'Wrong Mega Stage A benchmark schema.'}
    if([bool]$j.temporal_used){throw 'Temporal assistance unexpectedly used.'}
    if(-not[bool]$j.admission.reduced -or -not[bool]$j.admission.ui_protected){throw 'Admission smoke gate failed.'}
    if([uint64]$j.admission.admitted_bytes-ge[uint64]$j.admission.full_bytes){throw 'Admission did not physically reduce allocation.'}
    if([int]$j.governor.quality_actions_executed-lt 1){throw 'Unified governor made no physical quality mutation.'}
    if([int]$j.governor.memory_executed_actions-lt 1){throw 'Unified governor made no physical memory mutation.'}
    if(-not[bool]$j.physical_memory.residency_restored){throw 'Residency did not recover after smoke.'}
    if(-not[bool]$j.final_quality_full){throw 'Full quality was not recovered after smoke.'}
    Write-Host "Mega A smoke PASS: misses $([math]::Round(100*[double]$j.baseline.miss_ratio,1))% -> $([math]::Round(100*[double]$j.adaptive.miss_ratio,1))%; admission 4096 -> $([int]$j.admission.admitted_width" -ForegroundColor Green

    Step 'Launching Mega Stage A Benchmark Center'
    $ui=Join-Path $repo 'build\Release\arc-mega-stage-a-benchmark-ui.exe'
    $process=Start-Process -FilePath $ui -WorkingDirectory $repo -PassThru
    Start-Sleep -Milliseconds 800
    $process.Refresh()
    if($process.HasExited){throw "Mega Stage A benchmark UI exited immediately with code $($process.ExitCode)."}

    Write-Host 'ARC Mega Stage A Benchmark Center opened.' -ForegroundColor Green
    Write-Host 'All CPU/policy gates and integrated GPU smoke passed.' -ForegroundColor Green
    Write-Host 'Click Run 90s Mega A Acceptance.' -ForegroundColor Yellow
    Write-Host 'The UI retains PASS/FAIL and Results URL; PowerShell also stays open.' -ForegroundColor Green
}finally{Pop-Location}
