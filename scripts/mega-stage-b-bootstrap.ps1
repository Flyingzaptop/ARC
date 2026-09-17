param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/mega-stage-b-native-host',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Mega-B",
    [string]$ExpectedSha = '',
    [int]$Seconds = 90
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

if($env:OS-ne 'Windows_NT'){throw 'Mega Stage B acceptance requires Windows with D3D12.'}
if(-not(Get-Command git.exe -ErrorAction SilentlyContinue)){throw 'git.exe is required.'}
if($Seconds-lt 30 -or $Seconds-gt 300){throw 'Seconds must be between 30 and 300.'}
$repo=Join-Path $WorkRoot 'repo'
New-Item -ItemType Directory -Force $WorkRoot|Out-Null

Step 'Preparing ARC Mega Stage B checkout'
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
    if($ExpectedSha -and $sha-ne $ExpectedSha){throw "Mega Stage B SHA mismatch. Expected $ExpectedSha, got $sha"}

    $cmake=Ensure-CMake
    $ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if(-not(Test-Path -LiteralPath $ctest)){throw 'ctest.exe not found next to cmake.exe.'}
    Write-Host "Using CMake: $cmake"
    $env:VSLANG='1033'

    Step 'Building Mega Stage B: Native Host Adapter + Reference Renderer'
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if($LASTEXITCODE-ne 0){throw "CMake configure failed: $LASTEXITCODE"}
    & $cmake --build build --config Release --target `
        arc-dx12-host-adapter-tests `
        arc-runtime-event-bridge-tests `
        arc-runtime-integration-tests `
        arc-global-action-arbiter-tests `
        arc-unified-runtime-governor-tests `
        dx12-mega-stage-b-host-renderer
    if($LASTEXITCODE-ne 0){throw "Mega Stage B build failed: $LASTEXITCODE"}

    Step 'Running Stage 13 deterministic host-contract gates'
    & $ctest --test-dir build -C Release -R 'arc-(dx12-host-adapter|runtime-event-bridge|runtime-integration|global-action-arbiter|unified-runtime-governor)' --output-on-failure
    if($LASTEXITCODE-ne 0){throw "Mega Stage B host-contract tests failed: $LASTEXITCODE"}

    Step 'Running 30-second native renderer smoke'
    $exe=Join-Path $repo 'build\Release\dx12-mega-stage-b-host-renderer.exe'
    $smoke=Join-Path $repo 'traces\mega-stage-b-bootstrap-smoke.json'
    New-Item -ItemType Directory -Force (Split-Path -Parent $smoke)|Out-Null
    Remove-Item -LiteralPath $smoke -Force -ErrorAction SilentlyContinue
    $previous=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        & $exe --seconds 30 --probe-frames 4 --control-frames 8 --output $smoke 2>&1|ForEach-Object{Write-Host $_}
        $smokeExit=$LASTEXITCODE
    }finally{$ErrorActionPreference=$previous}
    if($smokeExit-eq 77){throw 'No suitable hardware D3D12 adapter was found.'}
    if($smokeExit-ne 0){throw "Mega Stage B native renderer smoke failed: $smokeExit"}
    if(-not(Test-Path -LiteralPath $smoke)){throw 'Mega Stage B smoke produced no JSON.'}
    $j=Get-Content -LiteralPath $smoke -Raw|ConvertFrom-Json
    if(-not[bool]$j.valid -or [string]$j.benchmark-ne 'mega_stage_b'){throw 'Mega Stage B smoke schema invalid.'}
    if([string]$j.integration-ne 'native_cooperative_dx12_host'){throw 'Native host boundary was not used.'}
    if([bool]$j.temporal_used){throw 'Temporal assistance unexpectedly used.'}
    if([int]$j.host.bridge_rejections-ne 0 -or [int]$j.host.failed_observations-ne 0 -or [int]$j.bridge.controller_rejections-ne 0){throw 'Host observation/control contract rejected events.'}
    if([int]$j.graph.errors-ne 0){throw 'ResourceGraph reported integration errors.'}
    if([int]$j.governor.quality_domains-lt 3){throw 'Smoke did not actuate all three renderer quality domains.'}
    if(-not[bool]$j.final_quality_full -or -not[bool]$j.residency_restored){throw 'Smoke did not fully recover quality/residency.'}
    Write-Host "Mega B smoke PASS: misses $([math]::Round(100*[double]$j.baseline.miss_ratio,1))% -> $([math]::Round(100*[double]$j.adaptive.miss_ratio,1))%; submits $([int]$j.host.queue_submits); presents $([int]$j.host.presents)" -ForegroundColor Green

    Step 'Running full Mega Stage B acceptance and publishing results'
    $runner=Join-Path $repo 'scripts\mega-stage-b-benchmark.ps1'
    $previous=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -RepoRoot $repo -Seconds $Seconds -ProbeFrames 12 -ControlFrames 24
        $acceptExit=$LASTEXITCODE
    }finally{$ErrorActionPreference=$previous}
    if($acceptExit-ne 0){throw "Mega Stage B acceptance failed with code $acceptExit. Check results\mega-stage-b-local\last-summary.txt"}

    $summary=Join-Path $repo 'results\mega-stage-b-local\last-summary.txt'
    if(Test-Path -LiteralPath $summary){Write-Host "`n$(Get-Content -LiteralPath $summary -Raw)"}
    Write-Host 'Mega Stage B acceptance PASS and results published.' -ForegroundColor Green
}finally{Pop-Location}
