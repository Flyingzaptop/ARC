param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(30,300)][int]$Seconds = 90,
    [ValidateRange(4,60)][int]$ProbeFrames = 12,
    [ValidateRange(4,120)][int]$ControlFrames = 24,
    [switch]$NoPublish,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object,[string]$Path){$Object|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $Path -Encoding utf8}
function Pct([double]$v){[math]::Round(100*$v,1)}
function R3([double]$v){[math]::Round($v,3)}

$RepoRoot=(Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha=(& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty=(& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if($dirty){throw 'Tracked source tree is dirty; refusing benchmark publication.'}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot=Join-Path $RepoRoot 'results\mega-stage-a-local'
$runDir=Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir|Out-Null
$log=Join-Path $runDir 'benchmark.log'
$benchmarkJson=Join-Path $runDir 'mega-stage-a.json'
$manifestJson=Join-Path $runDir 'manifest.json'
$acceptanceJson=Join-Path $runDir 'acceptance.json'
$summaryMd=Join-Path $runDir 'SUMMARY.md'
$lastSummary=Join-Path $localRoot 'last-summary.txt'
$lastUrl=Join-Path $localRoot 'last-result-url.txt'
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue
$exe=Join-Path $RepoRoot 'build\Release\dx12-mega-stage-a-benchmark.exe'
if(-not(Test-Path -LiteralPath $exe)){throw "Benchmark executable not found: $exe"}

$powerScheme='';$osCaption='';$osBuild='';$cpuName='';$driver=''
try{$powerScheme=(& powercfg.exe /getactivescheme 2>$null|Out-String).Trim()}catch{}
try{$os=Get-CimInstance Win32_OperatingSystem -ErrorAction Stop;$osCaption=[string]$os.Caption;$osBuild=[string]$os.BuildNumber}catch{}
try{$cpuName=[string](Get-CimInstance Win32_Processor -ErrorAction Stop|Select-Object -First 1 -ExpandProperty Name)}catch{}
try{$driver=[string](Get-CimInstance Win32_VideoController -ErrorAction Stop|Where-Object{$_.Name -match 'NVIDIA|AMD|Intel'}|Select-Object -First 1 -ExpandProperty DriverVersion)}catch{}

Step 'ARC Mega Stage A integrated acceptance'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds (+ calibration and recovery tail)"
Write-Host 'Stages 9-12: RuntimeIntegration + Admission + Physical Actuation + Global Governor'
Write-Host 'Native 1920x1080. Temporal / DLSS / FSR / Frame Generation / dynamic resolution: OFF'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow
$started=[DateTime]::UtcNow.ToString('o');$exit=-1;$fatal=$null;$previous=$ErrorActionPreference
try{$ErrorActionPreference='Continue';& $exe --seconds $Seconds --probe-frames $ProbeFrames --control-frames $ControlFrames --output $benchmarkJson 2>&1|Tee-Object -FilePath $log;$exit=$LASTEXITCODE;if($exit-ne 0){$fatal="Benchmark executable exit code $exit"}}catch{$fatal=$_.Exception.Message;($_|Out-String)|Add-Content -LiteralPath $log}finally{$ErrorActionPreference=$previous}
$finished=[DateTime]::UtcNow.ToString('o')
$bench=$null
if(Test-Path -LiteralPath $benchmarkJson){try{$bench=Get-Content -LiteralPath $benchmarkJson -Raw|ConvertFrom-Json}catch{$fatal="Invalid benchmark JSON: $($_.Exception.Message)"}}elseif(-not$fatal){$fatal='Benchmark JSON was not produced.'}
$manifest=[ordered]@{schema=1;source_sha=$sourceSha;started_utc=$started;finished_utc=$finished;seconds=$Seconds;probe_frames=$ProbeFrames;control_frames=$ControlFrames;temporal_enabled=$false;benchmark_exit_code=$exit;adapter=if($bench){[string]$bench.adapter}else{''};os=$osCaption;os_build=$osBuild;cpu=$cpuName;gpu_driver=$driver;power_scheme=$powerScheme;fatal_error=$fatal}
Json-Write $manifest $manifestJson

$gates=[ordered]@{
 json_valid=$false; mega_stage_a_schema=$false; native_1080p=$false; temporal_disabled=$false
 admission_reduced_physical_allocation=$false; critical_semantic_protected=$false
 runtime_quality_actuation=$false; online_effect_learning=$false; multi_domain_quality=$false
 physical_residency_cycle=$false; global_memory_path=$false; combined_arbitration=$false; restore_path=$false
 target_tracking_improved=$false; full_quality_recovered=$false; backend_clean=$false
}
$metrics=[ordered]@{}
if($bench){
 $bmiss=[double]$bench.baseline.miss_ratio;$amiss=[double]$bench.adaptive.miss_ratio
 $bo=[double]$bench.baseline.mean_overshoot_ms;$ao=[double]$bench.adaptive.mean_overshoot_ms
 $missReduction=if($bmiss-gt 0){($bmiss-$amiss)/$bmiss}else{0.0};$overReduction=if($bo-gt 0){($bo-$ao)/$bo}else{0.0}
 $fullBytes=[uint64]$bench.admission.full_bytes;$admittedBytes=[uint64]$bench.admission.admitted_bytes
 $memoryRelief=if([uint64]$bench.physical_memory.dxgi_before -gt [uint64]$bench.physical_memory.dxgi_min_after_evict){[uint64]$bench.physical_memory.dxgi_before-[uint64]$bench.physical_memory.dxgi_min_after_evict}else{0}
 $restoreRise=if([uint64]$bench.physical_memory.dxgi_final -gt [uint64]$bench.physical_memory.dxgi_min_after_evict){[uint64]$bench.physical_memory.dxgi_final-[uint64]$bench.physical_memory.dxgi_min_after_evict}else{0}
 $gates.json_valid=[bool]$bench.valid
 $gates.mega_stage_a_schema=([string]$bench.benchmark -eq 'mega_stage_a')
 $gates.native_1080p=([int]$bench.native_width-eq 1920 -and [int]$bench.native_height-eq 1080)
 $gates.temporal_disabled=(-not[bool]$bench.temporal_used)
 $gates.admission_reduced_physical_allocation=([bool]$bench.admission.reduced -and [int]$bench.admission.admitted_width-lt 4096 -and $admittedBytes-lt $fullBytes)
 $gates.critical_semantic_protected=[bool]$bench.admission.ui_protected
 $gates.runtime_quality_actuation=([int]$bench.governor.quality_actions_executed-ge 3)
 $gates.online_effect_learning=([int]$bench.governor.learned_effects-ge 2 -and [int]$bench.governor.pending_effects_resolved-ge 2)
 $gates.multi_domain_quality=([int]$bench.governor.quality_domains-ge 3)
 $gates.physical_residency_cycle=([bool]$bench.physical_memory.residency_restored -and $memoryRelief-ge (16MB) -and $restoreRise-ge (16MB))
 $gates.global_memory_path=([int]$bench.governor.memory_ticks-ge 1 -and [int]$bench.governor.memory_executed_actions-ge 2)
 $gates.combined_arbitration=([int]$bench.governor.combined_ticks-ge 1)
 $gates.restore_path=([int]$bench.governor.restore_ticks-ge 1)
 $gates.target_tracking_improved=(($missReduction-ge .15) -or ($overReduction-ge .20))
 $gates.full_quality_recovered=[bool]$bench.final_quality_full
 $gates.backend_clean=([int]$bench.governor.quality_backend_failures-eq 0)
 $metrics=[ordered]@{target_frame_ms=[double]$bench.target_frame_ms;baseline_p50_ms=[double]$bench.baseline.p50_ms;adaptive_p50_ms=[double]$bench.adaptive.p50_ms;baseline_p99_ms=[double]$bench.baseline.p99_ms;adaptive_p99_ms=[double]$bench.adaptive.p99_ms;baseline_miss_ratio=$bmiss;adaptive_miss_ratio=$amiss;miss_reduction_fraction=$missReduction;baseline_mean_overshoot_ms=$bo;adaptive_mean_overshoot_ms=$ao;overshoot_reduction_fraction=$overReduction;admission_level=[int]$bench.admission.level;admitted_width=[int]$bench.admission.admitted_width;physical_admission_saved_bytes=$fullBytes-$admittedBytes;quality_actions=[int]$bench.governor.quality_actions_executed;quality_domains=[int]$bench.governor.quality_domains;learned_effects=[int]$bench.governor.learned_effects;memory_actions=[int]$bench.governor.memory_executed_actions;combined_ticks=[int]$bench.governor.combined_ticks;restore_ticks=[int]$bench.governor.restore_ticks;physical_vram_relief_bytes=$memoryRelief;physical_vram_restore_bytes=$restoreRise}
}
$passed=(-not$fatal)-and($exit-eq 0);foreach($v in $gates.Values){$passed=$passed-and[bool]$v};$verdict=if($passed){'PASS'}else{'FAIL'}
$acceptance=[ordered]@{schema=1;verdict=$verdict;mega_stage_a_valid=[bool]$passed;gates=$gates;metrics=$metrics;fatal_error=$fatal};Json-Write $acceptance $acceptanceJson
$summary=@"
# ARC Mega Stage A - Stages 9-12

- Source: `$sourceSha`
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Native target: **1920x1080**
- Temporal / DLSS / FSR / Frame Generation / dynamic resolution: **OFF**

## Gates
- Runtime generic physical quality actuation: $($gates.runtime_quality_actuation)
- Resource admission physically reduced allocation: $($gates.admission_reduced_physical_allocation)
- UI/critical semantic protection: $($gates.critical_semantic_protected)
- Online effect learning: $($gates.online_effect_learning)
- Multi-domain quality control: $($gates.multi_domain_quality)
- Physical Evict -> MakeResident cycle: $($gates.physical_residency_cycle)
- Unified memory path: $($gates.global_memory_path)
- Combined memory+quality arbitration: $($gates.combined_arbitration)
- Restore path: $($gates.restore_path)
- Frame-budget tracking improved: $($gates.target_tracking_improved)
- Full quality recovered: $($gates.full_quality_recovered)
- Backend clean: $($gates.backend_clean)
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Benchmark complete'
Write-Host "Verdict       : $verdict"
if($bench){Write-Host "Budget misses : $(Pct([double]$metrics.baseline_miss_ratio))% -> $(Pct([double]$metrics.adaptive_miss_ratio))%";Write-Host "Miss reduction: $(Pct([double]$metrics.miss_reduction_fraction))%";Write-Host "Quality       : $($metrics.quality_actions) actions / $($metrics.quality_domains) domains / $($metrics.learned_effects) learned effects";Write-Host "Admission     : 4096 -> $($metrics.admitted_width), saved $([math]::Round([double]$metrics.physical_admission_saved_bytes/1MB,1)) MiB";Write-Host "Residency     : $($metrics.memory_actions) actions, relief $([math]::Round([double]$metrics.physical_vram_relief_bytes/1MB,1)) MiB, restore $([math]::Round([double]$metrics.physical_vram_restore_bytes/1MB,1)) MiB";Write-Host "Arbitration   : $($metrics.combined_ticks) combined / $($metrics.restore_ticks) restore ticks"}
Write-Host "Local results : $runDir"

$summaryText="ARC Mega Stage A $verdict`r`nStages 9-12 integrated`r`nResults URL:"
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8
$url='';$publishFailed=$false
if(-not$NoPublish){Step 'Publishing results';$publisher=Join-Path $PSScriptRoot 'mega-stage-a-publish-existing.ps1';try{& $publisher -RepoRoot $RepoRoot -RunStamp $stamp;if($LASTEXITCODE-ne 0){throw "Publisher exit code $LASTEXITCODE"};if(Test-Path -LiteralPath $lastUrl){$url=(Get-Content -LiteralPath $lastUrl -Raw).Trim()}}catch{$publishFailed=$true;Write-Host "Publishing failed: $($_.Exception.Message)" -ForegroundColor Red}}
$summaryText="ARC Mega Stage A $verdict`r`nBudget misses: $(if($bench){"$(Pct([double]$metrics.baseline_miss_ratio))% -> $(Pct([double]$metrics.adaptive_miss_ratio))%"}else{'n/a'})`r`nQuality: $(if($bench){"$($metrics.quality_actions) actions / $($metrics.quality_domains) domains"}else{'n/a'})`r`nAdmission physical: $($gates.admission_reduced_physical_allocation)`r`nResidency physical: $($gates.physical_residency_cycle)`r`nCombined arbitration: $($gates.combined_arbitration)`r`nFull quality recovered: $($gates.full_quality_recovered)`r`nTemporal: OFF`r`nResults URL: $url"
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8
$code=if($publishFailed){3}elseif($passed){0}else{2}
if($KeepOpen){Write-Host "`nBenchmark window will remain open." -ForegroundColor Cyan;Read-Host 'Press Enter to close'|Out-Null}
exit $code
