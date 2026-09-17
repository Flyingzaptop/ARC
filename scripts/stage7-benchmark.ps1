param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(6,600)][int]$Seconds = 60,
    [switch]$NoPublish,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8
}
function Write-LastResult([string]$Path, $Acceptance, [string]$Url) {
    $m = $Acceptance.metrics
    @(
        "verdict=$($Acceptance.verdict)",
        "baseline_p50_ms=$($m.baseline_p50_ms)",
        "adaptive_p50_ms=$($m.adaptive_p50_ms)",
        "p50_delta_ms=$($m.p50_delta_ms)",
        "baseline_p99_ms=$($m.baseline_p99_ms)",
        "adaptive_p99_ms=$($m.adaptive_p99_ms)",
        "p99_delta_ms=$($m.p99_delta_ms)",
        "selected_actions=$($m.selected_actions)",
        "texture_delta_ms=$($m.texture_delta_ms)",
        "geometry_delta_ms=$($m.geometry_delta_ms)",
        "raster_delta_ms=$($m.raster_delta_ms)",
        "lighting_delta_ms=$($m.lighting_delta_ms)",
        "shadow_delta_ms=$($m.shadow_delta_ms)",
        "url=$Url"
    ) | Set-Content -LiteralPath $Path -Encoding ascii
}

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha = (& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty = (& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if ($dirty) { throw 'Tracked source tree is dirty; refusing Stage 7 benchmark publication.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot = Join-Path $RepoRoot 'results\stage7-local'
$runDir = Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$log = Join-Path $runDir 'benchmark.log'
$benchmarkJson = Join-Path $runDir 'mixed-graphics-benchmark.json'
$manifestJson = Join-Path $runDir 'manifest.json'
$acceptanceJson = Join-Path $runDir 'acceptance.json'
$summaryMd = Join-Path $runDir 'SUMMARY.md'
$lastResult = Join-Path $localRoot 'last-result.txt'
$lastUrl = Join-Path $localRoot 'last-result-url.txt'

$benchmarkExe = Join-Path $RepoRoot 'build\Release\dx12-mixed-graphics-benchmark.exe'
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw "Benchmark executable not found: $benchmarkExe" }

$powerScheme = ''
try { $powerScheme = (& powercfg.exe /getactivescheme 2>$null | Out-String).Trim() } catch {}
$osCaption = ''
$osBuild = ''
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $osCaption = [string]$os.Caption
    $osBuild = [string]$os.BuildNumber
} catch {}
$cpuName = ''
try { $cpuName = [string](Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Name) } catch {}
$driver = ''
try {
    $driver = [string](Get-CimInstance Win32_VideoController -ErrorAction Stop |
        Where-Object { $_.Name -match 'NVIDIA|AMD|Intel' } |
        Select-Object -First 1 -ExpandProperty DriverVersion)
} catch {}

Step 'ARC Stage 7 Mixed Graphics Benchmark'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds"
Write-Host 'Native 100% graphics workload. Temporal / DLSS / FSR / Frame Generation: OFF'
Write-Host 'The benchmark measures separate shadow, geometry, raster, texture/bandwidth and lighting passes.'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow

$startedUtc = [DateTime]::UtcNow.ToString('o')
$exitCode = -1
$fatal = $null
$oldEap = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $benchmarkExe --seconds $Seconds --output $benchmarkJson 2>&1 | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { $fatal = "Benchmark executable exit code $exitCode" }
} catch {
    $fatal = $_.Exception.Message
    ($_ | Out-String) | Add-Content -LiteralPath $log
} finally {
    $ErrorActionPreference = $oldEap
}
$finishedUtc = [DateTime]::UtcNow.ToString('o')

$bench = $null
if (Test-Path -LiteralPath $benchmarkJson) {
    try { $bench = Get-Content -LiteralPath $benchmarkJson -Raw | ConvertFrom-Json } catch { $fatal = "Invalid benchmark JSON: $($_.Exception.Message)" }
} elseif (-not $fatal) {
    $fatal = 'Benchmark JSON was not produced.'
}

$manifest = [ordered]@{
    schema = 1
    stage = 7
    source_sha = $sourceSha
    started_utc = $startedUtc
    finished_utc = $finishedUtc
    seconds = $Seconds
    temporal_enabled = $false
    benchmark_exit_code = $exitCode
    adapter = if ($bench) { [string]$bench.adapter } else { '' }
    os = $osCaption
    os_build = $osBuild
    cpu = $cpuName
    gpu_driver = $driver
    power_scheme = $powerScheme
    fatal_error = $fatal
}
Json-Write $manifest $manifestJson

$gates = [ordered]@{
    json_valid = $false
    temporal_disabled = $false
    actions_selected = $false
    non_temporal_actions_only = $false
    physical_p50_improved = $false
    physical_p99_not_regressed = $false
    multiple_real_graphics_domains_measured = $false
    selected_action_has_measured_domain_effect = $false
}
$metrics = [ordered]@{}

if ($bench) {
    $actions = @($bench.actions)
    $baseP50 = [double]$bench.baseline.p50_ms
    $arcP50 = [double]$bench.adaptive.p50_ms
    $baseP99 = [double]$bench.baseline.p99_ms
    $arcP99 = [double]$bench.adaptive.p99_ms
    $improvementFraction = if ($baseP50 -gt 0) { ($baseP50 - $arcP50) / $baseP50 } else { 0.0 }
    $nonTemporal = @($actions | Where-Object { [int]$_.domain -eq 6 }).Count -eq 0

    $basePass = $bench.baseline.passes_p50_ms
    $arcPass = $bench.adaptive.passes_p50_ms
    $shadowDelta = [double]$arcPass.shadow - [double]$basePass.shadow
    $geometryDelta = [double]$arcPass.geometry - [double]$basePass.geometry
    $rasterDelta = [double]$arcPass.raster - [double]$basePass.raster
    $textureDelta = [double]$arcPass.texture - [double]$basePass.texture
    $lightingDelta = [double]$arcPass.lighting - [double]$basePass.lighting

    $measuredDomains = @($basePass.shadow,$basePass.geometry,$basePass.raster,$basePass.texture,$basePass.lighting | Where-Object { [double]$_ -gt 0.0 }).Count
    $effectful = $false
    foreach ($a in $actions) {
        switch ([int]$a.domain) {
            0 { if ($textureDelta -lt 0) { $effectful = $true } }
            1 { if ($textureDelta -lt 0) { $effectful = $true } }
            2 { if ($rasterDelta -lt 0) { $effectful = $true } }
            3 { if ($geometryDelta -lt 0) { $effectful = $true } }
            4 { if ($lightingDelta -lt 0) { $effectful = $true } }
            5 { if ($shadowDelta -lt 0) { $effectful = $true } }
        }
    }

    $gates.json_valid = [bool]$bench.valid
    $gates.temporal_disabled = (-not [bool]$bench.temporal_enabled) -and (-not [bool]$bench.temporal_used)
    $gates.actions_selected = $actions.Count -gt 0
    $gates.non_temporal_actions_only = $nonTemporal
    $gates.physical_p50_improved = ($arcP50 -lt $baseP50) -and ($improvementFraction -ge 0.02)
    $gates.physical_p99_not_regressed = $arcP99 -le ($baseP99 + 0.15)
    $gates.multiple_real_graphics_domains_measured = $measuredDomains -ge 5
    $gates.selected_action_has_measured_domain_effect = $effectful

    $metrics = [ordered]@{
        baseline_p50_ms = $baseP50
        adaptive_p50_ms = $arcP50
        p50_delta_ms = $arcP50 - $baseP50
        p50_improvement_fraction = $improvementFraction
        baseline_p99_ms = $baseP99
        adaptive_p99_ms = $arcP99
        p99_delta_ms = $arcP99 - $baseP99
        selected_actions = $actions.Count
        planned_gain_ms = [double]$bench.delta.planned_gain_ms
        planned_memory_freed_bytes = [uint64]$bench.delta.memory_freed_bytes
        shadow_delta_ms = $shadowDelta
        geometry_delta_ms = $geometryDelta
        raster_delta_ms = $rasterDelta
        texture_delta_ms = $textureDelta
        lighting_delta_ms = $lightingDelta
        baseline_dxgi_peak_usage = [uint64]$bench.baseline.dxgi_peak_usage
        adaptive_dxgi_peak_usage = [uint64]$bench.adaptive.dxgi_peak_usage
    }
}

$passed = (-not $fatal)
foreach ($value in $gates.Values) { $passed = $passed -and [bool]$value }
$verdict = if ($passed) { 'PASS' } else { 'FAIL' }
$acceptance = [ordered]@{
    schema = 1
    stage = 7
    verdict = $verdict
    mixed_graphics_core_valid = [bool]$passed
    gates = $gates
    metrics = $metrics
    fatal_error = $fatal
}
Json-Write $acceptance $acceptanceJson

$summary = @"
# ARC Stage 7 Mixed Graphics Benchmark

- Source: `$sourceSha`
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Temporal / DLSS / FSR / Frame Generation: **OFF**
- Duration: $Seconds s
- Power scheme: $powerScheme

## Physical gates
- JSON valid: $($gates.json_valid)
- Temporal disabled: $($gates.temporal_disabled)
- Non-temporal actions selected: $($gates.actions_selected)
- No temporal action selected: $($gates.non_temporal_actions_only)
- Total GPU P50 improved by >=2%: $($gates.physical_p50_improved)
- Total GPU P99 not regressed by >0.15 ms: $($gates.physical_p99_not_regressed)
- Five real graphics domains measured: $($gates.multiple_real_graphics_domains_measured)
- Selected action produced measured domain effect: $($gates.selected_action_has_measured_domain_effect)

This benchmark uses real D3D12 graphics PSOs and separates shadow, geometry, raster, texture/bandwidth and lighting GPU timings. It validates ARC's cross-domain native quality arbitration under repeatable conditions; it is not yet a claim about arbitrary closed games.
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Benchmark complete'
Write-Host "Verdict       : $verdict"
if ($bench) {
    Write-Host "Baseline P50  : $([math]::Round([double]$metrics.baseline_p50_ms,3)) ms"
    Write-Host "Adaptive P50  : $([math]::Round([double]$metrics.adaptive_p50_ms,3)) ms"
    Write-Host "P50 delta     : $([math]::Round([double]$metrics.p50_delta_ms,3)) ms"
    Write-Host "P99 delta     : $([math]::Round([double]$metrics.p99_delta_ms,3)) ms"
    Write-Host "Actions       : $($metrics.selected_actions)"
    Write-Host "Pass deltas   : shadow=$([math]::Round([double]$metrics.shadow_delta_ms,3)) geometry=$([math]::Round([double]$metrics.geometry_delta_ms,3)) raster=$([math]::Round([double]$metrics.raster_delta_ms,3)) texture=$([math]::Round([double]$metrics.texture_delta_ms,3)) lighting=$([math]::Round([double]$metrics.lighting_delta_ms,3)) ms"
}
Write-Host "Local results : $runDir"

$url = ''
if (-not $NoPublish) {
    Step 'Publishing results'
    $branch = "results/stage7-$stamp"
    $publishRoot = Join-Path $env:TEMP "arc-stage7-publish-$stamp"
    if (Test-Path -LiteralPath $publishRoot) { Remove-Item -LiteralPath $publishRoot -Recurse -Force }
    $publishError = $null
    $oldEap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & git.exe -C $RepoRoot branch $branch $sourceSha | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git branch failed: $LASTEXITCODE" }
        & git.exe -C $RepoRoot worktree add --force $publishRoot $branch | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed: $LASTEXITCODE" }

        $dest = Join-Path $publishRoot ("results\stage7\" + $stamp)
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item -Path (Join-Path $runDir '*') -Destination $dest -Recurse -Force -ErrorAction Stop
        & git.exe -C $publishRoot add -- "results/stage7/$stamp"
        if ($LASTEXITCODE -ne 0) { throw "git add failed: $LASTEXITCODE" }
        & git.exe -C $publishRoot commit -m "Publish Stage 7 mixed graphics benchmark $stamp" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git commit failed: $LASTEXITCODE" }
        & git.exe -C $publishRoot push -u origin $branch | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git push failed: $LASTEXITCODE" }

        $url = "https://github.com/Flyingzaptop/ARC/tree/$branch/results/stage7/$stamp"
        Set-Content -LiteralPath $lastUrl -Value $url -Encoding ascii
        Write-Host "`nResults URL: $url" -ForegroundColor Green
    } catch {
        $publishError = $_.Exception.Message
        Write-Host "Publishing failed: $publishError" -ForegroundColor Red
    } finally {
        $ErrorActionPreference = $oldEap
        try { & git.exe -C $RepoRoot worktree remove --force $publishRoot 2>$null | Out-Null } catch {}
    }
    if ($publishError) { $fatal = $publishError }
}

Write-LastResult $lastResult $acceptance $url
if ($KeepOpen) {
    Write-Host ''
    [void](Read-Host 'Benchmark finished. Press Enter to close this PowerShell window')
}
if ($fatal) { exit 3 }
if ($passed) { exit 0 } else { exit 2 }
