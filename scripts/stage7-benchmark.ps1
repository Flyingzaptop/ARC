param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(8,600)][int]$Seconds = 60,
    [ValidateRange(4,120)][int]$ProbeFrames = 18,
    [switch]$NoPublish
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object, [string]$Path) { $Object | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8 }
function Invoke-GitChecked([string[]]$Args, [string]$WorkingDir) {
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & git.exe -C $WorkingDir @Args 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($code -ne 0) { throw "git $($Args -join ' ') failed ($code): $($output -join ' ')" }
    return $output
}

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha = (& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty = (& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if ($dirty) { throw 'Tracked source tree is dirty; refusing benchmark publication.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot = Join-Path $RepoRoot 'results\stage7-local'
$runDir = Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$log = Join-Path $runDir 'benchmark.log'
$benchmarkJson = Join-Path $runDir 'mixed-graphics-benchmark.json'
$manifestJson = Join-Path $runDir 'manifest.json'
$acceptanceJson = Join-Path $runDir 'acceptance.json'
$summaryMd = Join-Path $runDir 'SUMMARY.md'
$lastSummary = Join-Path $localRoot 'last-summary.txt'
$lastUrl = Join-Path $localRoot 'last-result-url.txt'
New-Item -ItemType Directory -Force $localRoot | Out-Null
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue

$benchmarkExe = Join-Path $RepoRoot 'build\Release\dx12-mixed-graphics-benchmark.exe'
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw "Benchmark executable not found: $benchmarkExe" }

$powerScheme = ''
try { $powerScheme = (& powercfg.exe /getactivescheme 2>$null | Out-String).Trim() } catch {}
$osCaption = ''; $osBuild = ''; $cpuName = ''; $driver = ''
try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; $osCaption=[string]$os.Caption; $osBuild=[string]$os.BuildNumber } catch {}
try { $cpuName=[string](Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Name) } catch {}
try { $driver=[string](Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.Name -match 'NVIDIA|AMD|Intel' } | Select-Object -First 1 -ExpandProperty DriverVersion) } catch {}

Step 'ARC Stage 7 mixed graphics benchmark'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds (+ micro-probes)"
Write-Host 'Native 1920x1080. Temporal / DLSS / FSR / Frame Generation: OFF'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow

$startedUtc = [DateTime]::UtcNow.ToString('o')
$exitCode = -1
$fatal = $null
$previous = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $benchmarkExe --seconds $Seconds --probe-frames $ProbeFrames --output $benchmarkJson 2>&1 | Tee-Object -FilePath $log
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
    schema=1; source_sha=$sourceSha; started_utc=$startedUtc; finished_utc=$finishedUtc; seconds=$Seconds; probe_frames=$ProbeFrames
    temporal_enabled=$false; benchmark_exit_code=$exitCode; adapter=if($bench){[string]$bench.adapter}else{''}
    os=$osCaption; os_build=$osBuild; cpu=$cpuName; gpu_driver=$driver; power_scheme=$powerScheme; fatal_error=$fatal
}
Json-Write $manifest $manifestJson

$gates = [ordered]@{
    json_valid=$false
    mixed_graphics=$false
    native_1080p=$false
    temporal_disabled=$false
    micro_probes_valid=$false
    actions_selected=$false
    multiple_domains_selected=$false
    physical_p50_improved=$false
    physical_p99_not_regressed=$false
}
$metrics = [ordered]@{}

if ($bench) {
    $actions=@($bench.actions); $probes=@($bench.probes)
    $baseP50=[double]$bench.baseline.p50_ms; $arcP50=[double]$bench.adaptive.p50_ms
    $baseP99=[double]$bench.baseline.p99_ms; $arcP99=[double]$bench.adaptive.p99_ms
    $improvement = if($baseP50 -gt 0){($baseP50-$arcP50)/$baseP50}else{0.0}
    $domains = @($actions | ForEach-Object { [int]$_.domain } | Sort-Object -Unique)
    $positiveProbes = @($probes | Where-Object { [double]$_.measured_gain_ms -gt 0.0 }).Count

    $gates.json_valid=[bool]$bench.valid
    $gates.mixed_graphics=([string]$bench.benchmark -eq 'mixed_graphics')
    $gates.native_1080p=([int]$bench.native_width -eq 1920 -and [int]$bench.native_height -eq 1080)
    $gates.temporal_disabled=(-not [bool]$bench.temporal_enabled) -and (-not [bool]$bench.temporal_used)
    $gates.micro_probes_valid=($probes.Count -ge 5 -and $positiveProbes -ge 2)
    $gates.actions_selected=($actions.Count -gt 0)
    $gates.multiple_domains_selected=($domains.Count -ge 2)
    $gates.physical_p50_improved=($arcP50 -lt $baseP50 -and $improvement -ge 0.03)
    $gates.physical_p99_not_regressed=($arcP99 -le ($baseP99 + 0.15))

    $metrics=[ordered]@{
        baseline_p50_ms=$baseP50; adaptive_p50_ms=$arcP50; p50_delta_ms=$arcP50-$baseP50; p50_improvement_fraction=$improvement
        baseline_p99_ms=$baseP99; adaptive_p99_ms=$arcP99; p99_delta_ms=$arcP99-$baseP99
        selected_actions=$actions.Count; selected_domains=$domains.Count; probes=$probes.Count; positive_probes=$positiveProbes
        planned_gain_ms=[double]$bench.delta.planned_gain_ms; visual_cost=[double]$bench.delta.visual_cost
        baseline_dxgi_peak_usage=[uint64]$bench.baseline.dxgi_peak_usage; adaptive_dxgi_peak_usage=[uint64]$bench.adaptive.dxgi_peak_usage
    }
}

$passed=(-not $fatal)
foreach($v in $gates.Values){ $passed=$passed -and [bool]$v }
$verdict=if($passed){'PASS'}else{'FAIL'}
$acceptance=[ordered]@{schema=1; verdict=$verdict; mixed_graphics_core_valid=[bool]$passed; gates=$gates; metrics=$metrics; fatal_error=$fatal}
Json-Write $acceptance $acceptanceJson

$summary=@"
# ARC Stage 7 Mixed Graphics Benchmark

- Source: `$sourceSha`
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Native target: **1920x1080**
- Temporal / DLSS / FSR / Frame Generation: **OFF**
- Duration: $Seconds s + per-domain micro-probes

## Gates
- JSON valid: $($gates.json_valid)
- Mixed graphics path: $($gates.mixed_graphics)
- Native 1080p: $($gates.native_1080p)
- Temporal disabled: $($gates.temporal_disabled)
- Measured micro-probes valid: $($gates.micro_probes_valid)
- Actions selected: $($gates.actions_selected)
- Multiple quality domains selected: $($gates.multiple_domains_selected)
- Physical GPU P50 improved by >=3%: $($gates.physical_p50_improved)
- Physical GPU P99 not regressed by >0.15 ms: $($gates.physical_p99_not_regressed)

This benchmark uses real D3D12 graphics passes for geometry, raster/overdraw, texture sampling, lighting and shadow work. ARC measures each candidate action on the local GPU before planning the adaptive phase.
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Benchmark complete'
Write-Host "Verdict       : $verdict"
if($bench){
    Write-Host "Baseline P50  : $([math]::Round([double]$metrics.baseline_p50_ms,3)) ms"
    Write-Host "Adaptive P50  : $([math]::Round([double]$metrics.adaptive_p50_ms,3)) ms"
    Write-Host "P50 delta     : $([math]::Round([double]$metrics.p50_delta_ms,3)) ms"
    Write-Host "P99 delta     : $([math]::Round([double]$metrics.p99_delta_ms,3)) ms"
    Write-Host "Actions       : $($metrics.selected_actions) across $($metrics.selected_domains) domains"
}
Write-Host "Local results : $runDir"

$url=''
if(-not $NoPublish){
    Step 'Publishing results'
    $branch="results/stage7-$stamp"
    $publishRoot=Join-Path $env:TEMP "arc-stage7-publish-$stamp"
    if(Test-Path -LiteralPath $publishRoot){ Remove-Item -LiteralPath $publishRoot -Recurse -Force }
    try {
        Invoke-GitChecked @('branch',$branch,$sourceSha) $RepoRoot | Out-Null
        Invoke-GitChecked @('worktree','add','--force',$publishRoot,$branch) $RepoRoot | Out-Null
        $dest=Join-Path $publishRoot ("results\stage7\"+$stamp)
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item -Path (Join-Path $runDir '*') -Destination $dest -Recurse -Force
        Invoke-GitChecked @('add','--',"results/stage7/$stamp") $publishRoot | Out-Null
        Invoke-GitChecked @('commit','-m',"Publish Stage 7 mixed graphics benchmark $stamp") $publishRoot | Out-Null
        Invoke-GitChecked @('push','-u','origin',$branch) $publishRoot | Out-Null
        $url="https://github.com/Flyingzaptop/ARC/tree/$branch/results/stage7/$stamp"
        Set-Content -LiteralPath $lastUrl -Value $url -Encoding ascii
        Write-Host "`nResults URL: $url" -ForegroundColor Green
    } catch {
        $fatalPublish=$_.Exception.Message
        Write-Host "Publishing failed: $fatalPublish" -ForegroundColor Red
    } finally {
        try { $previous=$ErrorActionPreference; $ErrorActionPreference='Continue'; & git.exe -C $RepoRoot worktree remove --force $publishRoot 2>$null | Out-Null; $ErrorActionPreference=$previous } catch {}
    }
}

$summaryText = if($bench){
    "ARC Stage 7 $verdict`r`nBaseline P50: $([math]::Round([double]$metrics.baseline_p50_ms,3)) ms`r`nAdaptive P50: $([math]::Round([double]$metrics.adaptive_p50_ms,3)) ms`r`nP50 delta: $([math]::Round([double]$metrics.p50_delta_ms,3)) ms`r`nP99 delta: $([math]::Round([double]$metrics.p99_delta_ms,3)) ms`r`nActions: $($metrics.selected_actions) across $($metrics.selected_domains) domains`r`nTemporal: OFF`r`nResults URL: $url"
}else{
    "ARC Stage 7 $verdict`r`nFatal: $fatal`r`nResults URL: $url"
}
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8

if($NoPublish){ if($passed){exit 0}else{exit 2} }
if($passed){exit 0}else{exit 2}
