param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(30,300)][int]$Seconds = 90,
    [ValidateRange(4,60)][int]$ProbeFrames = 12,
    [ValidateRange(4,120)][int]$ControlFrames = 24,
    [switch]$NoPublish,
    [switch]$KeepOpen
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
function Step([string]$Text){Write-Host "`n=== $Text ===" -ForegroundColor Cyan}
function Json-Write($Object,[string]$Path){$Object|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $Path -Encoding utf8}
function Pct([double]$v){[math]::Round(100*$v,1)}

$RepoRoot=(Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha=(& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty=(& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if($dirty){throw 'Tracked source tree is dirty; refusing benchmark publication.'}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot=Join-Path $RepoRoot 'results\mega-stage-b-local'
$runDir=Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir|Out-Null
$log=Join-Path $runDir 'benchmark.log'
$benchmarkJson=Join-Path $runDir 'mega-stage-b.json'
$manifestJson=Join-Path $runDir 'manifest.json'
$acceptanceJson=Join-Path $runDir 'acceptance.json'
$summaryMd=Join-Path $runDir 'SUMMARY.md'
$lastSummary=Join-Path $localRoot 'last-summary.txt'
$lastUrl=Join-Path $localRoot 'last-result-url.txt'
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue

$exe=Join-Path $RepoRoot 'build\Release\dx12-mega-stage-b-host-renderer.exe'
$hostTest=Join-Path $RepoRoot 'build\Release\arc-dx12-host-adapter-tests.exe'
$arbiterTest=Join-Path $RepoRoot 'build\Release\arc-global-action-arbiter-tests.exe'
$governorTest=Join-Path $RepoRoot 'build\Release\arc-unified-runtime-governor-tests.exe'
foreach($path in @($exe,$hostTest,$arbiterTest,$governorTest)){if(-not(Test-Path -LiteralPath $path)){throw "Required Mega Stage B executable not found: $path"}}

$powerScheme='';$osCaption='';$osBuild='';$cpuName='';$driver=''
try{$powerScheme=(& powercfg.exe /getactivescheme 2>$null|Out-String).Trim()}catch{}
try{$os=Get-CimInstance Win32_OperatingSystem -ErrorAction Stop;$osCaption=[string]$os.Caption;$osBuild=[string]$os.BuildNumber}catch{}
try{$cpuName=[string](Get-CimInstance Win32_Processor -ErrorAction Stop|Select-Object -First 1 -ExpandProperty Name)}catch{}
try{$driver=[string](Get-CimInstance Win32_VideoController -ErrorAction Stop|Where-Object{$_.Name -match 'NVIDIA|AMD|Intel'}|Select-Object -First 1 -ExpandProperty DriverVersion)}catch{}

Step 'Mega Stage B deterministic integration gates'
$previous=$ErrorActionPreference
try{
    $ErrorActionPreference='Continue'
    & $hostTest 2>&1|Tee-Object -FilePath $log
    $hostTestExit=$LASTEXITCODE
    & $arbiterTest 2>&1|Tee-Object -FilePath $log -Append
    $arbiterExit=$LASTEXITCODE
    & $governorTest 2>&1|Tee-Object -FilePath $log -Append
    $governorExit=$LASTEXITCODE
}finally{$ErrorActionPreference=$previous}
$policyPassed=($hostTestExit-eq 0 -and $arbiterExit-eq 0 -and $governorExit-eq 0)
if(-not$policyPassed){Write-Host "Deterministic gates failed: host=$hostTestExit arbiter=$arbiterExit governor=$governorExit" -ForegroundColor Red}

Step 'ARC Mega Stage B native renderer acceptance'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds per baseline/adaptive schedule (+ calibration/recovery)"
Write-Host 'Stage 13: Native cooperative D3D12 Host Adapter'
Write-Host 'Stage 14: Win32/DXGI reference renderer integration'
Write-Host 'Native 1920x1080. Temporal / DLSS / FSR / Frame Generation / dynamic resolution: OFF'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow
$started=[DateTime]::UtcNow.ToString('o');$exit=-1;$fatal=$null
try{
    $ErrorActionPreference='Continue'
    & $exe --seconds $Seconds --probe-frames $ProbeFrames --control-frames $ControlFrames --output $benchmarkJson 2>&1|Tee-Object -FilePath $log -Append
    $exit=$LASTEXITCODE
    if($exit-ne 0){$fatal="Benchmark executable exit code $exit"}
} catch {
    $fatal=$_.Exception.Message
    ($_|Out-String)|Add-Content -LiteralPath $log
} finally {$ErrorActionPreference='Stop'}
$finished=[DateTime]::UtcNow.ToString('o')

$bench=$null
if(Test-Path -LiteralPath $benchmarkJson){
    try{$bench=Get-Content -LiteralPath $benchmarkJson -Raw|ConvertFrom-Json}catch{$fatal="Invalid benchmark JSON: $($_.Exception.Message)"}
}elseif(-not$fatal){$fatal='Benchmark JSON was not produced.'}

$manifest=[ordered]@{
    schema=1;source_sha=$sourceSha;started_utc=$started;finished_utc=$finished
    seconds=$Seconds;probe_frames=$ProbeFrames;control_frames=$ControlFrames
    temporal_enabled=$false;benchmark_exit_code=$exit
    host_adapter_test_exit=$hostTestExit;combined_arbiter_test_exit=$arbiterExit;combined_governor_test_exit=$governorExit
    adapter=if($bench){[string]$bench.adapter}else{''}
    os=$osCaption;os_build=$osBuild;cpu=$cpuName;gpu_driver=$driver;power_scheme=$powerScheme;fatal_error=$fatal
}
Json-Write $manifest $manifestJson

$gates=[ordered]@{
    json_valid=$false
    mega_stage_b_schema=$false
    native_cooperative_host=$false
    native_1080p=$false
    temporal_disabled=$false
    deterministic_host_contract=$false
    resource_lifecycle_observed=$false
    descriptor_path_observed=$false
    command_submission_observed=$false
    fence_completion_observed=$false
    presentation_observed=$false
    resource_graph_clean=$false
    observation_control_separation_clean=$false
    physical_quality_actuation=$false
    online_effect_learning=$false
    multi_domain_quality=$false
    global_memory_path=$false
    combined_arbitration=$false
    restore_path=$false
    target_tracking_improved=$false
    full_quality_recovered=$false
    residency_recovered=$false
    backend_clean=$false
}
$metrics=[ordered]@{}
if($bench){
    $bmiss=[double]$bench.baseline.miss_ratio;$amiss=[double]$bench.adaptive.miss_ratio
    $bo=[double]$bench.baseline.mean_overshoot_ms;$ao=[double]$bench.adaptive.mean_overshoot_ms
    $missReduction=if($bmiss-gt 0){($bmiss-$amiss)/$bmiss}else{0.0}
    $overReduction=if($bo-gt 0){($bo-$ao)/$bo}else{0.0}
    $gates.json_valid=[bool]$bench.valid
    $gates.mega_stage_b_schema=([string]$bench.benchmark-eq 'mega_stage_b')
    $gates.native_cooperative_host=([string]$bench.integration-eq 'native_cooperative_dx12_host')
    $gates.native_1080p=([int]$bench.native_width-eq 1920 -and [int]$bench.native_height-eq 1080)
    $gates.temporal_disabled=(-not[bool]$bench.temporal_used)
    $gates.deterministic_host_contract=$policyPassed
    $gates.resource_lifecycle_observed=([int]$bench.host.resources-ge 5 -and [int]$bench.graph.resources-ge 5)
    $gates.descriptor_path_observed=([int]$bench.host.descriptor_writes-ge 3)
    $gates.command_submission_observed=([int]$bench.host.queue_submits-ge 20 -and [int]$bench.host.resource_uses-ge 20)
    $gates.fence_completion_observed=([int]$bench.host.fence_signals-ge 20 -and [int]$bench.host.completion_updates-ge 20)
    $gates.presentation_observed=([int]$bench.host.presents-ge 20 -and [int64]$bench.graph.presentation_frame-ge 20)
    $gates.resource_graph_clean=([int]$bench.graph.errors-eq 0)
    $gates.observation_control_separation_clean=([int]$bench.host.bridge_rejections-eq 0 -and [int]$bench.host.failed_observations-eq 0 -and [int]$bench.bridge.controller_rejections-eq 0 -and [int]$bench.bridge.malformed-eq 0)
    $gates.physical_quality_actuation=([int]$bench.governor.quality_actions_executed-ge 3)
    $gates.online_effect_learning=([int]$bench.governor.learned_effects-ge 2 -and [int]$bench.governor.pending_effects_resolved-ge 2)
    $gates.multi_domain_quality=([int]$bench.governor.quality_domains-ge 3)
    $gates.global_memory_path=([int]$bench.governor.memory_ticks-ge 1 -and [int]$bench.governor.memory_executed_actions-ge 2)
    $gates.combined_arbitration=([int]$bench.governor.combined_ticks-ge 1 -or ($arbiterExit-eq 0 -and $governorExit-eq 0))
    $gates.restore_path=([int]$bench.governor.restore_ticks-ge 1)
    $gates.target_tracking_improved=(($missReduction-ge .10) -or ($overReduction-ge .15))
    $gates.full_quality_recovered=[bool]$bench.final_quality_full
    $gates.residency_recovered=[bool]$bench.residency_restored
    $gates.backend_clean=([int]$bench.governor.quality_backend_failures-eq 0)
    $metrics=[ordered]@{
        target_frame_ms=[double]$bench.target_frame_ms
        baseline_p50_ms=[double]$bench.baseline.p50_ms;adaptive_p50_ms=[double]$bench.adaptive.p50_ms
        baseline_p99_ms=[double]$bench.baseline.p99_ms;adaptive_p99_ms=[double]$bench.adaptive.p99_ms
        baseline_miss_ratio=$bmiss;adaptive_miss_ratio=$amiss;miss_reduction_fraction=$missReduction
        baseline_mean_overshoot_ms=$bo;adaptive_mean_overshoot_ms=$ao;overshoot_reduction_fraction=$overReduction
        host_resources=[int]$bench.host.resources;descriptor_writes=[int]$bench.host.descriptor_writes
        resource_uses=[int]$bench.host.resource_uses;queue_submits=[int]$bench.host.queue_submits
        fence_signals=[int]$bench.host.fence_signals;completion_updates=[int]$bench.host.completion_updates
        presents=[int]$bench.host.presents;events_drained=[int64]$bench.host.events_drained
        quality_actions=[int]$bench.governor.quality_actions_executed;quality_domains=[int]$bench.governor.quality_domains
        learned_effects=[int]$bench.governor.learned_effects;memory_actions=[int]$bench.governor.memory_executed_actions
        combined_ticks=[int]$bench.governor.combined_ticks;restore_ticks=[int]$bench.governor.restore_ticks
    }
}

$passed=(-not$fatal)-and($exit-eq 0)
foreach($v in $gates.Values){$passed=$passed-and[bool]$v}
$verdict=if($passed){'PASS'}else{'FAIL'}
$acceptance=[ordered]@{schema=1;verdict=$verdict;mega_stage_b_valid=[bool]$passed;gates=$gates;metrics=$metrics;fatal_error=$fatal}
Json-Write $acceptance $acceptanceJson

$summary=@"
# ARC Mega Stage B - Stages 13-14

- Source: $sourceSha
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Integration: **native cooperative D3D12 host**
- Reference renderer: **Win32 + DXGI swapchain + D3D12**
- Native target: **1920x1080**
- Temporal / DLSS / FSR / Frame Generation / dynamic resolution: **OFF**

## Stage 13 - Native Host Adapter
- Deterministic host contract: $($gates.deterministic_host_contract)
- Resource lifecycle observed: $($gates.resource_lifecycle_observed)
- Descriptor path observed: $($gates.descriptor_path_observed)
- Command submission observed: $($gates.command_submission_observed)
- Fence completion observed: $($gates.fence_completion_observed)
- Presentation observed: $($gates.presentation_observed)
- ResourceGraph clean: $($gates.resource_graph_clean)
- Observation/control separation clean: $($gates.observation_control_separation_clean)

## Stage 14 - Integrated Reference Renderer
- Physical quality actuation: $($gates.physical_quality_actuation)
- Online effect learning: $($gates.online_effect_learning)
- Multi-domain quality: $($gates.multi_domain_quality)
- Global memory path: $($gates.global_memory_path)
- Combined arbitration: $($gates.combined_arbitration)
- Restore path: $($gates.restore_path)
- Target tracking improved: $($gates.target_tracking_improved)
- Full quality recovered: $($gates.full_quality_recovered)
- Residency recovered: $($gates.residency_recovered)
- Backend clean: $($gates.backend_clean)
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Mega Stage B benchmark complete'
Write-Host "Verdict       : $verdict"
if($bench){
    Write-Host "Budget misses : $(Pct([double]$metrics.baseline_miss_ratio))% -> $(Pct([double]$metrics.adaptive_miss_ratio))%"
    Write-Host "Miss reduction: $(Pct([double]$metrics.miss_reduction_fraction))%"
    Write-Host "Host path     : $($metrics.host_resources) resources / $($metrics.queue_submits) submits / $($metrics.presents) presents"
    Write-Host "Quality       : $($metrics.quality_actions) actions / $($metrics.quality_domains) domains / $($metrics.learned_effects) learned"
    Write-Host "Memory        : $($metrics.memory_actions) actions / $($metrics.combined_ticks) combined / $($metrics.restore_ticks) restore ticks"
}
Write-Host "Local results : $runDir"
$summaryText="ARC Mega Stage B $verdict`r`nStages 13-14 integrated`r`nResults URL:"
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8
$url='';$publishFailed=$false
if(-not$NoPublish){
    Step 'Publishing results'
    $publisher=Join-Path $PSScriptRoot 'mega-stage-b-publish-existing.ps1'
    try{
        & $publisher -RepoRoot $RepoRoot -RunStamp $stamp
        if($LASTEXITCODE-ne 0){throw "Publisher exit code $LASTEXITCODE"}
        if(Test-Path -LiteralPath $lastUrl){$url=(Get-Content -LiteralPath $lastUrl -Raw).Trim()}
    }catch{$publishFailed=$true;Write-Host "Publishing failed: $($_.Exception.Message)" -ForegroundColor Red}
}
$summaryText="ARC Mega Stage B $verdict`r`nBudget misses: $(if($bench){"$(Pct([double]$metrics.baseline_miss_ratio))% -> $(Pct([double]$metrics.adaptive_miss_ratio))%"}else{'n/a'})`r`nNative host clean: $($gates.observation_control_separation_clean)`r`nRenderer command path: $($gates.command_submission_observed)`r`nQuality domains: $(if($bench){$metrics.quality_domains}else{'n/a'})`r`nFull recovery: $($gates.full_quality_recovered)`r`nTemporal: OFF`r`nResults URL: $url"
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8
$code=if($publishFailed){3}elseif($passed){0}else{2}
if($KeepOpen){Write-Host "`nBenchmark window will remain open." -ForegroundColor Cyan;Read-Host 'Press Enter to close'|Out-Null}
exit $code
