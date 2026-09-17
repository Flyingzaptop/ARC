param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(12,600)][int]$Seconds = 60,
    [ValidateRange(4,120)][int]$ProbeFrames = 12,
    [ValidateRange(4,240)][int]$ControlFrames = 24,
    [switch]$NoPublish,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object, [string]$Path) { $Object | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding utf8 }
function Round3([double]$Value) { return [math]::Round($Value,3) }
function Percent1([double]$Value) { return [math]::Round(100.0*$Value,1) }

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha = (& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty = (& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if ($dirty) { throw 'Tracked source tree is dirty; refusing benchmark publication.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot = Join-Path $RepoRoot 'results\stage8-local'
$runDir = Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$log = Join-Path $runDir 'benchmark.log'
$benchmarkJson = Join-Path $runDir 'closed-loop-mixed-graphics.json'
$manifestJson = Join-Path $runDir 'manifest.json'
$acceptanceJson = Join-Path $runDir 'acceptance.json'
$summaryMd = Join-Path $runDir 'SUMMARY.md'
$lastSummary = Join-Path $localRoot 'last-summary.txt'
$lastUrl = Join-Path $localRoot 'last-result-url.txt'
New-Item -ItemType Directory -Force $localRoot | Out-Null
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue

$benchmarkExe = Join-Path $RepoRoot 'build\Release\dx12-closed-loop-mixed-graphics-benchmark.exe'
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw "Benchmark executable not found: $benchmarkExe" }

$powerScheme = ''
try { $powerScheme = (& powercfg.exe /getactivescheme 2>$null | Out-String).Trim() } catch {}
$osCaption = ''; $osBuild = ''; $cpuName = ''; $driver = ''
try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; $osCaption=[string]$os.Caption; $osBuild=[string]$os.BuildNumber } catch {}
try { $cpuName=[string](Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Name) } catch {}
try { $driver=[string](Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.Name -match 'NVIDIA|AMD|Intel' } | Select-Object -First 1 -ExpandProperty DriverVersion) } catch {}

Step 'ARC Stage 8 closed-loop mixed graphics benchmark'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds (+ calibration probes)"
Write-Host 'Native 1920x1080. Temporal / DLSS / FSR / Frame Generation / dynamic resolution: OFF'
Write-Host 'The dynamic scene schedule is repeated: fixed full quality, then ARC closed loop.'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow

$startedUtc = [DateTime]::UtcNow.ToString('o')
$exitCode = -1
$fatal = $null
$previous = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $benchmarkExe --seconds $Seconds --probe-frames $ProbeFrames --control-frames $ControlFrames --output $benchmarkJson 2>&1 | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { $fatal = "Benchmark executable exit code $exitCode" }
} catch {
    $fatal = $_.Exception.Message
    ($_ | Out-String) | Add-Content -LiteralPath $log
} finally { $ErrorActionPreference = $previous }
$finishedUtc = [DateTime]::UtcNow.ToString('o')

$bench = $null
if (Test-Path -LiteralPath $benchmarkJson) {
    try { $bench = Get-Content -LiteralPath $benchmarkJson -Raw | ConvertFrom-Json } catch { $fatal = "Invalid benchmark JSON: $($_.Exception.Message)" }
} elseif (-not $fatal) { $fatal = 'Benchmark JSON was not produced.' }

$manifest = [ordered]@{
    schema=1; source_sha=$sourceSha; started_utc=$startedUtc; finished_utc=$finishedUtc
    seconds=$Seconds; probe_frames=$ProbeFrames; control_frames=$ControlFrames
    temporal_enabled=$false; benchmark_exit_code=$exitCode
    adapter=if($bench){[string]$bench.adapter}else{''}
    os=$osCaption; os_build=$osBuild; cpu=$cpuName; gpu_driver=$driver; power_scheme=$powerScheme; fatal_error=$fatal
}
Json-Write $manifest $manifestJson

$gates = [ordered]@{
    json_valid=$false
    closed_loop_mixed_graphics=$false
    native_1080p=$false
    temporal_disabled=$false
    scene_transitions_valid=$false
    quality_ladders_measured=$false
    live_degrade_observed=$false
    live_restore_observed=$false
    multi_domain_adaptation=$false
    target_tracking_improved=$false
    quality_recovered=$false
    no_chatter=$false
}
$metrics = [ordered]@{}

if ($bench) {
    $probes=@($bench.quality_probes)
    $scenes=@($bench.scene_calibration)
    $events=@($bench.events)
    $positiveProbes=@($probes | Where-Object { [double]$_.measured_gain_ms -gt 0.0 })
    $probeDomains=@($probes | ForEach-Object { [int]$_.domain } | Sort-Object -Unique)
    $positiveDomains=@($positiveProbes | ForEach-Object { [int]$_.domain } | Sort-Object -Unique)
    $heavyScenes=@($scenes | Where-Object { [double]$_.full_quality_p50_ms -gt [double]$bench.target_frame_ms })
    $degradeEvents=@($events | Where-Object { [string]$_.kind -eq 'degrade' })
    $restoreEvents=@($events | Where-Object { [string]$_.kind -eq 'restore' })

    $baselineMiss=[double]$bench.baseline.miss_ratio
    $adaptiveMiss=[double]$bench.adaptive.miss_ratio
    $baselineOvershoot=[double]$bench.baseline.mean_overshoot_ms
    $adaptiveOvershoot=[double]$bench.adaptive.mean_overshoot_ms
    $missReduction=[double]$bench.delta.miss_reduction_fraction
    $overshootReduction=[double]$bench.delta.overshoot_reduction_fraction
    $finalActive=[int]$bench.controller.final_active_actions
    $uniqueDomains=[int]$bench.controller.unique_degraded_domains
    $maxPhaseFlips=[int]$bench.controller.max_phase_direction_flips

    $full=$bench.full_quality
    $final=$bench.final_quality
    $qualityRestored=(
        [int]$full.geometry_instances -eq [int]$final.geometry_instances -and
        [int]$full.raster_layers -eq [int]$final.raster_layers -and
        [int]$full.texture_samples -eq [int]$final.texture_samples -and
        [int]$full.light_iterations -eq [int]$final.light_iterations -and
        [int]$full.shadow_passes -eq [int]$final.shadow_passes -and
        [int]$full.shadow_samples -eq [int]$final.shadow_samples)

    $trackingImproved=$false
    if ($baselineMiss -gt 0.0 -and $missReduction -ge 0.15) { $trackingImproved=$true }
    if ($baselineOvershoot -gt 0.0 -and $overshootReduction -ge 0.20) { $trackingImproved=$true }

    $gates.json_valid=[bool]$bench.valid
    $gates.closed_loop_mixed_graphics=([string]$bench.benchmark -eq 'closed_loop_mixed_graphics')
    $gates.native_1080p=([int]$bench.native_width -eq 1920 -and [int]$bench.native_height -eq 1080)
    $gates.temporal_disabled=(-not [bool]$bench.temporal_enabled) -and (-not [bool]$bench.temporal_used)
    $gates.scene_transitions_valid=($scenes.Count -ge 5 -and $heavyScenes.Count -ge 5)
    $gates.quality_ladders_measured=($probes.Count -ge 15 -and $probeDomains.Count -ge 5 -and $positiveProbes.Count -ge 5 -and $positiveDomains.Count -ge 3)
    $gates.live_degrade_observed=($degradeEvents.Count -ge 3)
    $gates.live_restore_observed=($restoreEvents.Count -ge 2)
    $gates.multi_domain_adaptation=($uniqueDomains -ge 3)
    $gates.target_tracking_improved=$trackingImproved
    $gates.quality_recovered=($finalActive -eq 0 -and $qualityRestored)
    $gates.no_chatter=($maxPhaseFlips -le 1)

    $metrics=[ordered]@{
        target_frame_ms=[double]$bench.target_frame_ms
        easy_full_quality_p50_ms=[double]$bench.easy_full_quality_p50_ms
        baseline_p50_ms=[double]$bench.baseline.p50_ms
        adaptive_p50_ms=[double]$bench.adaptive.p50_ms
        baseline_p99_ms=[double]$bench.baseline.p99_ms
        adaptive_p99_ms=[double]$bench.adaptive.p99_ms
        baseline_miss_ratio=$baselineMiss
        adaptive_miss_ratio=$adaptiveMiss
        miss_reduction_fraction=$missReduction
        baseline_mean_overshoot_ms=$baselineOvershoot
        adaptive_mean_overshoot_ms=$adaptiveOvershoot
        overshoot_reduction_fraction=$overshootReduction
        degrade_events=$degradeEvents.Count
        restore_events=$restoreEvents.Count
        unique_degraded_domains=$uniqueDomains
        direction_changes=[int]$bench.controller.direction_changes
        max_phase_direction_flips=$maxPhaseFlips
        final_active_actions=$finalActive
        quality_probes=$probes.Count
        positive_quality_probes=$positiveProbes.Count
        probed_domains=$probeDomains.Count
        baseline_dxgi_peak_usage=[uint64]$bench.baseline.dxgi_peak_usage
        adaptive_dxgi_peak_usage=[uint64]$bench.adaptive.dxgi_peak_usage
    }
}

$passed=(-not $fatal) -and ($exitCode -eq 0)
foreach($v in $gates.Values){ $passed=$passed -and [bool]$v }
$verdict=if($passed){'PASS'}else{'FAIL'}
$acceptance=[ordered]@{schema=1; verdict=$verdict; closed_loop_quality_valid=[bool]$passed; gates=$gates; metrics=$metrics; fatal_error=$fatal}
Json-Write $acceptance $acceptanceJson

$summary=@"
# ARC Stage 8 Closed-Loop Adaptive Graphics

- Source: `$sourceSha`
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Native target: **1920x1080**
- Temporal / DLSS / FSR / Frame Generation / dynamic resolution: **OFF**
- Dynamic schedule: fixed full quality baseline -> ARC closed loop

## Acceptance
- Dynamic heavy scenes valid: $($gates.scene_transitions_valid)
- 3-step quality ladders measured: $($gates.quality_ladders_measured)
- Live degrade observed: $($gates.live_degrade_observed)
- Live restore observed: $($gates.live_restore_observed)
- Multi-domain adaptation: $($gates.multi_domain_adaptation)
- Frame-budget tracking improved: $($gates.target_tracking_improved)
- Full quality recovered at end: $($gates.quality_recovered)
- No per-phase chatter: $($gates.no_chatter)
- Temporal disabled: $($gates.temporal_disabled)

Stage 8 intentionally judges target tracking and reversible quality recovery rather than requiring a lower aggregate P50. Easy phases should restore quality instead of staying permanently degraded.
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Benchmark complete'
Write-Host "Verdict            : $verdict"
if($bench){
    Write-Host "Target             : $(Round3([double]$metrics.target_frame_ms)) ms"
    Write-Host "Baseline misses    : $(Percent1([double]$metrics.baseline_miss_ratio))%"
    Write-Host "ARC misses         : $(Percent1([double]$metrics.adaptive_miss_ratio))%"
    Write-Host "Miss reduction     : $(Percent1([double]$metrics.miss_reduction_fraction))%"
    Write-Host "Overshoot reduction: $(Percent1([double]$metrics.overshoot_reduction_fraction))%"
    Write-Host "Actions            : $($metrics.degrade_events) degrade / $($metrics.restore_events) restore; $($metrics.unique_degraded_domains) domains"
    Write-Host "Final active       : $($metrics.final_active_actions)"
}
Write-Host "Local results      : $runDir"

$url=''
$publishFailed=$false
$summaryText = if($bench){
    "ARC Stage 8 $verdict`r`nTarget: $(Round3([double]$metrics.target_frame_ms)) ms`r`nBaseline budget misses: $(Percent1([double]$metrics.baseline_miss_ratio))%`r`nARC budget misses: $(Percent1([double]$metrics.adaptive_miss_ratio))%`r`nMiss reduction: $(Percent1([double]$metrics.miss_reduction_fraction))%`r`nOvershoot reduction: $(Percent1([double]$metrics.overshoot_reduction_fraction))%`r`nActions: $($metrics.degrade_events) degrade / $($metrics.restore_events) restore across $($metrics.unique_degraded_domains) domains`r`nQuality recovered: $($gates.quality_recovered)`r`nTemporal: OFF`r`nResults URL:"
}else{
    "ARC Stage 8 $verdict`r`nFatal: $fatal`r`nResults URL:"
}
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8

if(-not $NoPublish){
    Step 'Publishing results'
    $publisher = Join-Path $PSScriptRoot 'stage8-publish-existing.ps1'
    try {
        if (-not (Test-Path -LiteralPath $publisher)) { throw "Publisher script not found: $publisher" }
        & $publisher -RepoRoot $RepoRoot -RunStamp $stamp
        if ($LASTEXITCODE -ne 0) { throw "Publisher exit code $LASTEXITCODE" }
        if (Test-Path -LiteralPath $lastUrl) { $url=(Get-Content -LiteralPath $lastUrl -Raw).Trim() }
    } catch {
        $publishFailed=$true
        Write-Host "Publishing failed: $($_.Exception.Message)" -ForegroundColor Red
    }
}

if($bench){
    $summaryText = "ARC Stage 8 $verdict`r`nTarget: $(Round3([double]$metrics.target_frame_ms)) ms`r`nBaseline budget misses: $(Percent1([double]$metrics.baseline_miss_ratio))%`r`nARC budget misses: $(Percent1([double]$metrics.adaptive_miss_ratio))%`r`nMiss reduction: $(Percent1([double]$metrics.miss_reduction_fraction))%`r`nOvershoot reduction: $(Percent1([double]$metrics.overshoot_reduction_fraction))%`r`nActions: $($metrics.degrade_events) degrade / $($metrics.restore_events) restore across $($metrics.unique_degraded_domains) domains`r`nQuality recovered: $($gates.quality_recovered)`r`nTemporal: OFF`r`nResults URL: $url"
} else {
    $summaryText = "ARC Stage 8 $verdict`r`nFatal: $fatal`r`nResults URL: $url"
}
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8

$finalCode = if($publishFailed){3}elseif($passed){0}else{2}
if($KeepOpen){
    Write-Host "`nBenchmark window will remain open." -ForegroundColor Cyan
    Read-Host 'Press Enter to close' | Out-Null
}
exit $finalCode
