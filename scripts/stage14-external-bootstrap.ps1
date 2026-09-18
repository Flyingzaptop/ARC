param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage14-external-renderer',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage14-External",
    [string]$ExpectedSha = '',
    [ValidateRange(10,120)][int]$Seconds = 30
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$UpstreamUrl='https://github.com/microsoft/DirectX-Graphics-Samples.git'
$UpstreamSha='213dd4fd4918ea009dd8f35adee1aff1f2ecaba4'

function Step([string]$Text){Write-Host ([Environment]::NewLine+"=== $Text ===") -ForegroundColor Cyan}

function Find-CMake {
    $cmd=Get-Command cmake.exe -ErrorAction SilentlyContinue
    if($cmd){return $cmd.Source}
    foreach($candidate in @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )){
        if($candidate -and(Test-Path -LiteralPath $candidate)){return $candidate}
    }
    $pf86=[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere=Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
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

function Find-MSBuild {
    $cmd=Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if($cmd){return $cmd.Source}
    $pf86=[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere=Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(-not(Test-Path -LiteralPath $vswhere)){return $null}
    $installation=(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1)
    if(-not$installation){return $null}
    foreach($candidate in @(
        (Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path $installation 'MSBuild\15.0\Bin\MSBuild.exe')
    )){
        if(Test-Path -LiteralPath $candidate){return $candidate}
    }
    return $null
}

function Ensure-NuGet([string]$ToolsRoot) {
    $cmd=Get-Command nuget.exe -ErrorAction SilentlyContinue
    if($cmd){return $cmd.Source}
    New-Item -ItemType Directory -Force $ToolsRoot | Out-Null
    $nuget=Join-Path $ToolsRoot 'nuget.exe'
    if(-not(Test-Path -LiteralPath $nuget)){
        Step 'Downloading NuGet CLI'
        Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
    }
    return $nuget
}

function Json-Write($Object,[string]$Path) {
    $Object | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Pct([double]$Value){return [math]::Round(100.0*$Value,1)}

if($env:OS-ne 'Windows_NT'){throw 'Stage 14 external renderer acceptance requires Windows with D3D12.'}
if(-not(Get-Command git.exe -ErrorAction SilentlyContinue)){throw 'git.exe is required.'}

$arcRepo=Join-Path $WorkRoot 'arc'
$externalRepo=Join-Path $WorkRoot 'DirectX-Graphics-Samples'
$toolsRoot=Join-Path $WorkRoot 'tools'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null

Step 'Preparing ARC checkout'
if(-not(Test-Path -LiteralPath (Join-Path $arcRepo '.git'))){
    & git.exe clone $RepoUrl $arcRepo
    if($LASTEXITCODE-ne 0){throw "ARC git clone failed: $LASTEXITCODE"}
}
& git.exe -C $arcRepo fetch origin $Branch --prune
if($LASTEXITCODE-ne 0){throw "ARC git fetch failed: $LASTEXITCODE"}
& git.exe -C $arcRepo reset --hard
& git.exe -C $arcRepo clean -fdx
& git.exe -C $arcRepo switch -C $Branch ("origin/"+$Branch)
if($LASTEXITCODE-ne 0){throw "ARC git switch failed: $LASTEXITCODE"}
$sourceSha=(& git.exe -C $arcRepo rev-parse HEAD).Trim()
Write-Host "ARC source SHA: $sourceSha"
if($ExpectedSha -and $sourceSha-ne$ExpectedSha){throw "ARC SHA mismatch. Expected $ExpectedSha, got $sourceSha"}

$cmake=Ensure-CMake
$ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if(-not(Test-Path -LiteralPath $ctest)){throw 'ctest.exe not found next to cmake.exe.'}
$msbuild=Find-MSBuild
if(-not$msbuild){throw 'MSBuild was not found. Install Visual Studio Build Tools with C++ desktop workload.'}
$nuget=Ensure-NuGet $toolsRoot
$env:VSLANG='1033'

Step 'Building ARC anti-chatter core + Native Host Adapter'
Push-Location $arcRepo
try{
    & $cmake -S . -B build -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
    if($LASTEXITCODE-ne 0){throw "ARC CMake configure failed: $LASTEXITCODE"}
    & $cmake --build build --config Release
    if($LASTEXITCODE-ne 0){throw "ARC Release build failed: $LASTEXITCODE"}

    Step 'Running deterministic ARC regressions'
    & $ctest --test-dir build -C Release -R 'arc-(adaptive-quality-tests|adaptive-quality-controller-tests|closed-loop-quality-sim-tests|dx12-host-adapter-tests|dx12-host-adapter-surface-tests|runtime-event-bridge-tests|runtime-integration-tests|global-action-arbiter-tests|unified-runtime-governor-tests)' --output-on-failure --timeout 60
    if($LASTEXITCODE-ne 0){throw "ARC deterministic regressions failed: $LASTEXITCODE"}
}finally{
    Pop-Location
}

Step 'Preparing pinned Microsoft DirectX external renderer'
if(-not(Test-Path -LiteralPath (Join-Path $externalRepo '.git'))){
    & git.exe clone --filter=blob:none --no-checkout $UpstreamUrl $externalRepo
    if($LASTEXITCODE-ne 0){throw "DirectX-Graphics-Samples clone failed: $LASTEXITCODE"}
    & git.exe -C $externalRepo sparse-checkout init --cone
    if($LASTEXITCODE-ne 0){throw "sparse-checkout init failed: $LASTEXITCODE"}
}
& git.exe -C $externalRepo reset --hard
& git.exe -C $externalRepo clean -fdx
& git.exe -C $externalRepo sparse-checkout set Samples/Desktop/D3D12HelloWorld/src/HelloTexture
if($LASTEXITCODE-ne 0){throw "sparse-checkout set failed: $LASTEXITCODE"}
& git.exe -C $externalRepo fetch origin $UpstreamSha --depth=1
if($LASTEXITCODE-ne 0){throw "Pinned upstream fetch failed: $LASTEXITCODE"}
& git.exe -C $externalRepo checkout --detach $UpstreamSha
if($LASTEXITCODE-ne 0){throw "Pinned upstream checkout failed: $LASTEXITCODE"}
$actualUpstream=(& git.exe -C $externalRepo rev-parse HEAD).Trim()
if($actualUpstream-ne$UpstreamSha){throw "Pinned upstream mismatch: $actualUpstream"}

$overlay=Join-Path $arcRepo 'scripts\apply-directx-hello-texture-integration.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $overlay -ExternalRoot $externalRepo -ArcRepoRoot $arcRepo
if($LASTEXITCODE-ne 0){throw "External renderer overlay failed: $LASTEXITCODE"}

$sampleRoot=Join-Path $externalRepo 'Samples\Desktop\D3D12HelloWorld\src\HelloTexture'
$project=Join-Path $sampleRoot 'D3D12HelloTexture.vcxproj'
$packages=Join-Path (Split-Path -Parent $sampleRoot) 'packages'

Step 'Restoring Microsoft sample packages'
& $nuget restore $project -PackagesDirectory $packages -NonInteractive
if($LASTEXITCODE-ne 0){throw "NuGet restore failed: $LASTEXITCODE"}

Step 'Building patched Microsoft D3D12HelloTexture'
& $msbuild $project /m /t:Build /p:Configuration=Release /p:Platform=x64 "/p:ArcRepoRoot=$arcRepo" /verbosity:minimal
if($LASTEXITCODE-ne 0){throw "External Microsoft renderer build failed: $LASTEXITCODE"}

$exe=Join-Path $sampleRoot 'bin\x64\Release\D3D12HelloTexture.exe'
if(-not(Test-Path -LiteralPath $exe)){throw "External renderer executable not found: $exe"}

$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot=Join-Path $arcRepo 'results\stage14-external-local'
$runDir=Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$rawJson=Join-Path $runDir 'external-renderer.json'
$manifestJson=Join-Path $runDir 'manifest.json'
$acceptanceJson=Join-Path $runDir 'acceptance.json'
$summaryMd=Join-Path $runDir 'SUMMARY.md'
$log=Join-Path $runDir 'benchmark.log'
$lastSummary=Join-Path $localRoot 'last-summary.txt'
$lastUrl=Join-Path $localRoot 'last-result-url.txt'
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue

Step 'Running external Microsoft D3D12 renderer acceptance'
Write-Host "Upstream: microsoft/DirectX-Graphics-Samples @ $UpstreamSha"
Write-Host "Renderer: Samples/Desktop/D3D12HelloWorld/src/HelloTexture"
Write-Host "ARC source: $sourceSha"
Write-Host "Native: 1920x1080; temporal/upscaling/frame-generation: OFF"
Write-Host "Baseline: $Seconds s; adaptive: $Seconds s; plus warmup/recovery"
Write-Host 'Leave other GPU-heavy applications idle.' -ForegroundColor Yellow

$env:ARC_EXTERNAL_SECONDS=[string]$Seconds
$env:ARC_EXTERNAL_CONTROL_FRAMES='16'
$env:ARC_EXTERNAL_WARMUP_SECONDS='3'
$env:ARC_EXTERNAL_RECOVERY_SECONDS='12'
$env:ARC_EXTERNAL_OUTPUT=$rawJson
$env:ARC_SOURCE_SHA=$sourceSha

$started=[DateTime]::UtcNow.ToString('o')
$process=$null
$exit=-1
$fatal=$null
try{
    $process=Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru
    $timeoutSeconds=($Seconds*2)+90
    if(-not$process.WaitForExit($timeoutSeconds*1000)){
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $fatal="External renderer timed out after $timeoutSeconds seconds."
    }else{
        $exit=$process.ExitCode
        if($exit-ne 0){$fatal="External renderer exit code $exit"}
    }
}catch{
    $fatal=$_.Exception.Message
}
$finished=[DateTime]::UtcNow.ToString('o')

$raw=$null
if(Test-Path -LiteralPath $rawJson){
    try{$raw=Get-Content -LiteralPath $rawJson -Raw | ConvertFrom-Json}
    catch{$fatal="Invalid external renderer JSON: $($_.Exception.Message)"}
}else{
    if(-not$fatal){$fatal='External renderer did not produce JSON.'}
    Json-Write ([ordered]@{
        schema=1
        valid=$false
        benchmark='stage14_external_renderer'
        integration='microsoft_directx_graphics_samples_hello_texture'
        upstream_sha=$UpstreamSha
        arc_source_sha=$sourceSha
        fatal_error=$fatal
    }) $rawJson
}

$powerScheme='';$osCaption='';$osBuild='';$cpuName='';$driver=''
try{$powerScheme=(& powercfg.exe /getactivescheme 2>$null | Out-String).Trim()}catch{}
try{$os=Get-CimInstance Win32_OperatingSystem -ErrorAction Stop;$osCaption=[string]$os.Caption;$osBuild=[string]$os.BuildNumber}catch{}
try{$cpuName=[string](Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Name)}catch{}
try{$driver=[string](Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object{$_.Name -match 'NVIDIA|AMD|Intel'} | Select-Object -First 1 -ExpandProperty DriverVersion)}catch{}

$manifest=[ordered]@{
    schema=1
    source_sha=$sourceSha
    upstream_repository='microsoft/DirectX-Graphics-Samples'
    upstream_sha=$UpstreamSha
    upstream_actual_sha=$actualUpstream
    started_utc=$started
    finished_utc=$finished
    seconds_per_measurement_phase=$Seconds
    benchmark_exit_code=$exit
    os=$osCaption
    os_build=$osBuild
    cpu=$cpuName
    gpu_driver=$driver
    power_scheme=$powerScheme
    fatal_error=$fatal
}
Json-Write $manifest $manifestJson

$gates=[ordered]@{
    report_parsed=$false
    report_self_valid=$false
    exact_arc_source=$false
    exact_pinned_upstream=$false
    external_renderer_identity=$false
    native_1080p=$false
    temporal_disabled=$false
    gpu_timestamp_timing=$false
    meaningful_baseline_pressure=$false
    resource_lifecycle_observed=$false
    descriptor_path_observed=$false
    command_submission_observed=$false
    fence_completion_observed=$false
    presentation_observed=$false
    resource_graph_clean=$false
    observation_control_separation_clean=$false
    physical_quality_actuation=$false
    online_effect_learning=$false
    miss_ratio_improved=$false
    p50_improved=$false
    p99_guard=$false
    bounded_churn=$false
    full_quality_recovered=$false
    backend_clean=$false
}
$metrics=[ordered]@{}

if($raw){
    $bmiss=[double]$raw.baseline.miss_ratio
    $amiss=[double]$raw.adaptive.miss_ratio
    $bp50=[double]$raw.baseline.p50_ms
    $ap50=[double]$raw.adaptive.p50_ms
    $bp99=[double]$raw.baseline.p99_ms
    $ap99=[double]$raw.adaptive.p99_ms
    $missReduction=if($bmiss-gt 0){($bmiss-$amiss)/$bmiss}else{0.0}
    $p50Reduction=if($bp50-gt 0){($bp50-$ap50)/$bp50}else{0.0}

    $gates.report_parsed=$true
    $gates.report_self_valid=[bool]$raw.valid
    $gates.exact_arc_source=([string]$raw.arc_source_sha-eq$sourceSha)
    $gates.exact_pinned_upstream=([string]$raw.upstream_sha-eq$UpstreamSha -and $actualUpstream-eq$UpstreamSha)
    $gates.external_renderer_identity=([string]$raw.benchmark-eq'stage14_external_renderer' -and [string]$raw.integration-eq'microsoft_directx_graphics_samples_hello_texture')
    $gates.native_1080p=([int]$raw.native_width-eq1920 -and [int]$raw.native_height-eq1080)
    $gates.temporal_disabled=(-not[bool]$raw.temporal_used)
    $gates.gpu_timestamp_timing=(
        [string]$raw.timing_source-eq'gpu_timestamp' -and
        [int64]$raw.gpu_timestamp_samples-ge([int64]$raw.baseline.samples+[int64]$raw.adaptive.samples)
    )
    $gates.meaningful_baseline_pressure=($bmiss-ge0.45 -and $bmiss-le0.75)
    $gates.resource_lifecycle_observed=([int]$raw.host.resources-ge4)
    $gates.descriptor_path_observed=([int]$raw.host.descriptor_writes-ge3)
    $gates.command_submission_observed=([int]$raw.host.queue_submits-ge100 -and [int]$raw.host.resource_uses-ge300)
    $gates.fence_completion_observed=([int]$raw.host.fence_signals-ge100 -and [int]$raw.host.completion_updates-ge100)
    $gates.presentation_observed=([int]$raw.host.presents-ge100)
    $gates.resource_graph_clean=([int]$raw.graph.errors-eq0)
    $gates.observation_control_separation_clean=([int]$raw.host.failed_observations-eq0 -and [int]$raw.host.bridge_rejections-eq0)
    $gates.physical_quality_actuation=([int]$raw.governor.quality_actions_executed-ge2)
    $gates.online_effect_learning=([int]$raw.governor.learned_effects-ge1 -and [int]$raw.governor.pending_effects_resolved-ge1)
    $gates.miss_ratio_improved=($missReduction-ge0.10)
    $gates.p50_improved=($p50Reduction-ge0.05)
    $gates.p99_guard=($bp99-gt0 -and $ap99-le($bp99*1.10))
    $gates.bounded_churn=(
        [bool]$raw.bounded_churn -and
        [int]$raw.governor.adaptive_quality_actions-le24 -and
        [double]$raw.governor.adaptive_action_rate-le0.010 -and
        [double]$raw.governor.adaptive_direction_change_rate-le0.005
    )
    $gates.full_quality_recovered=[bool]$raw.final_quality_full
    $gates.backend_clean=([int]$raw.governor.quality_backend_failures-eq0)

    $metrics=[ordered]@{
        target_frame_ms=[double]$raw.target_frame_ms
        gpu_timestamp_samples=[int64]$raw.gpu_timestamp_samples
        baseline_p50_ms=$bp50
        adaptive_p50_ms=$ap50
        p50_reduction_fraction=$p50Reduction
        baseline_p99_ms=$bp99
        adaptive_p99_ms=$ap99
        baseline_miss_ratio=$bmiss
        adaptive_miss_ratio=$amiss
        miss_reduction_fraction=$missReduction
        baseline_mean_overshoot_ms=[double]$raw.baseline.mean_overshoot_ms
        adaptive_mean_overshoot_ms=[double]$raw.adaptive.mean_overshoot_ms
        resources=[int]$raw.host.resources
        resource_uses=[int]$raw.host.resource_uses
        queue_submits=[int]$raw.host.queue_submits
        presents=[int]$raw.host.presents
        events_drained=[int64]$raw.host.events_drained
        quality_actions=[int]$raw.governor.quality_actions_executed
        direction_changes=[int]$raw.governor.direction_changes
        adaptive_quality_actions=[int]$raw.governor.adaptive_quality_actions
        adaptive_direction_changes=[int]$raw.governor.adaptive_direction_changes
        adaptive_action_rate=[double]$raw.governor.adaptive_action_rate
        adaptive_direction_change_rate=[double]$raw.governor.adaptive_direction_change_rate
        learned_effects=[int]$raw.governor.learned_effects
        restore_probes=[int]$raw.governor.restore_probes
        restore_backoffs=[int]$raw.governor.restore_backoffs
    }
}

$passed=(-not$fatal)-and($exit-eq0)
foreach($value in $gates.Values){$passed=$passed-and[bool]$value}
$verdict=if($passed){'PASS'}else{'FAIL'}

$acceptance=[ordered]@{
    schema=1
    verdict=$verdict
    stage14_external_valid=[bool]$passed
    gates=$gates
    metrics=$metrics
    fatal_error=$fatal
}
Json-Write $acceptance $acceptanceJson

$summary=@"
# ARC Stage 14 External Renderer Acceptance

- ARC source: $sourceSha
- External source: microsoft/DirectX-Graphics-Samples @ $UpstreamSha
- Renderer: D3D12HelloTexture (patched at acceptance time; upstream not vendored)
- Verdict: **$verdict**
- Native target: **1920x1080**
- Temporal / upscaling / frame generation / dynamic resolution: **OFF**
- Timing source: **D3D12 GPU timestamp queries**

## Integration
- Exact pinned upstream: $($gates.exact_pinned_upstream)
- Resource lifecycle observed: $($gates.resource_lifecycle_observed)
- Descriptor path observed: $($gates.descriptor_path_observed)
- Command submission observed: $($gates.command_submission_observed)
- Fence completion observed: $($gates.fence_completion_observed)
- Presentation observed: $($gates.presentation_observed)
- ResourceGraph clean: $($gates.resource_graph_clean)
- Observation/control separation clean: $($gates.observation_control_separation_clean)
- GPU timestamp timing: $($gates.gpu_timestamp_timing)

## Performance
- Meaningful baseline pressure: $($gates.meaningful_baseline_pressure)
- Physical quality actuation: $($gates.physical_quality_actuation)
- Online effect learning: $($gates.online_effect_learning)
- Miss ratio improved >= 10%: $($gates.miss_ratio_improved)
- p50 improved >= 5%: $($gates.p50_improved)
- p99 regression <= 10%: $($gates.p99_guard)
- Bounded governor churn: $($gates.bounded_churn) (adaptive action rate <= 1.0%, direction-change rate <= 0.5% of control ticks)
- Full quality recovered: $($gates.full_quality_recovered)
- Backend clean: $($gates.backend_clean)
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

$logLines=@(
    "ARC Stage 14 external renderer acceptance",
    "ARC source: $sourceSha",
    "Upstream: microsoft/DirectX-Graphics-Samples @ $UpstreamSha",
    "Executable: $exe",
    "Exit code: $exit",
    "Verdict: $verdict"
)
if($raw){
    $logLines+=@(
        "Adapter: $([string]$raw.adapter)",
        "Target: $([double]$raw.target_frame_ms) ms",
        "Baseline miss: $(Pct([double]$raw.baseline.miss_ratio))%",
        "Adaptive miss: $(Pct([double]$raw.adaptive.miss_ratio))%",
        "Baseline p50: $([double]$raw.baseline.p50_ms) ms",
        "Adaptive p50: $([double]$raw.adaptive.p50_ms) ms",
        "Quality actions: $([int]$raw.governor.quality_actions_executed)",
        "Direction changes: $([int]$raw.governor.direction_changes)",
        "Restore probes/backoffs: $([int]$raw.governor.restore_probes)/$([int]$raw.governor.restore_backoffs)",
        "Full quality: $([bool]$raw.final_quality_full)"
    )
}
$logLines | Set-Content -LiteralPath $log -Encoding utf8

Step 'Stage 14 external acceptance complete'
Write-Host "Verdict        : $verdict"
if($raw){
    Write-Host "Budget misses  : $(Pct([double]$raw.baseline.miss_ratio))% -> $(Pct([double]$raw.adaptive.miss_ratio))%"
    Write-Host "p50            : $([math]::Round([double]$raw.baseline.p50_ms,3)) -> $([math]::Round([double]$raw.adaptive.p50_ms,3)) ms"
    Write-Host "p99            : $([math]::Round([double]$raw.baseline.p99_ms,3)) -> $([math]::Round([double]$raw.adaptive.p99_ms,3)) ms"
    Write-Host "Quality actions: $([int]$raw.governor.quality_actions_executed); direction changes: $([int]$raw.governor.direction_changes)"
}
Write-Host "Local results  : $runDir"

$summaryText="ARC Stage 14 External $verdict"+[Environment]::NewLine+
    $(if($raw){"Misses: $(Pct([double]$raw.baseline.miss_ratio))% -> $(Pct([double]$raw.adaptive.miss_ratio))%"}else{'Misses: n/a'})+
    [Environment]::NewLine+
    $(if($raw){"p50: $([math]::Round([double]$raw.baseline.p50_ms,3)) -> $([math]::Round([double]$raw.adaptive.p50_ms,3)) ms"}else{'p50: n/a'})+
    [Environment]::NewLine+"Results URL:"
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8

$publishFailed=$false
Step 'Publishing external acceptance'
try{
    $publisher=Join-Path $arcRepo 'scripts\stage14-external-publish-existing.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $publisher -RepoRoot $arcRepo -RunStamp $stamp
    if($LASTEXITCODE-ne0){throw "Publisher exit code $LASTEXITCODE"}
}catch{
    $publishFailed=$true
    Write-Host "Publishing failed: $($_.Exception.Message)" -ForegroundColor Red
}

if(Test-Path -LiteralPath $lastSummary){
    Write-Host ([Environment]::NewLine+(Get-Content -LiteralPath $lastSummary -Raw))
}

if($publishFailed){exit 3}
if($passed){exit 0}
exit 2
