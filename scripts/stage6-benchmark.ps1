param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(6,600)][int]$Seconds = 60,
    [switch]$NoPublish
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $Path -Encoding utf8
}

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$sourceSha = (& git.exe -C $RepoRoot rev-parse HEAD).Trim()
$dirty = (& git.exe -C $RepoRoot status --porcelain --untracked-files=no)
if ($dirty) { throw 'Tracked source tree is dirty; refusing benchmark publication.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot = Join-Path $RepoRoot 'results\stage6-local'
$runDir = Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$log = Join-Path $runDir 'benchmark.log'
$benchmarkJson = Join-Path $runDir 'adaptive-quality-benchmark.json'
$manifestJson = Join-Path $runDir 'manifest.json'
$acceptanceJson = Join-Path $runDir 'acceptance.json'
$summaryMd = Join-Path $runDir 'SUMMARY.md'

$benchmarkExe = Join-Path $RepoRoot 'build\Release\dx12-adaptive-quality-benchmark.exe'
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

Step 'ARC Stage 6 deterministic GPU benchmark'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds"
Write-Host 'Native workload. Temporal / DLSS / FSR / Frame Generation: OFF'
Write-Host 'Leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow

$startedUtc = [DateTime]::UtcNow.ToString('o')
$exitCode = -1
$fatal = $null
try {
    & $benchmarkExe --seconds $Seconds --output $benchmarkJson 2>&1 | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { $fatal = "Benchmark executable exit code $exitCode" }
} catch {
    $fatal = $_.Exception.Message
    ($_ | Out-String) | Add-Content -LiteralPath $log
}
$finishedUtc = [DateTime]::UtcNow.ToString('o')

$bench = $null
if (Test-Path -LiteralPath $benchmarkJson) {
    try { $bench = Get-Content -LiteralPath $benchmarkJson -Raw | ConvertFrom-Json } catch { $fatal = "Invalid benchmark JSON: $($_.Exception.Message)" }
} elseif (-not $fatal) {
    $fatal = 'Benchmark JSON was not produced.'
}

$manifest = [ordered]@{
    schema = 2
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

    $gates.json_valid = [bool]$bench.valid
    $gates.temporal_disabled = (-not [bool]$bench.temporal_enabled) -and (-not [bool]$bench.temporal_used)
    $gates.actions_selected = $actions.Count -gt 0
    $gates.non_temporal_actions_only = $nonTemporal
    $gates.physical_p50_improved = ($arcP50 -lt $baseP50) -and ($improvementFraction -ge 0.02)
    $gates.physical_p99_not_regressed = $arcP99 -le ($baseP99 + 0.10)

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
    }
}

$passed = (-not $fatal)
foreach ($value in $gates.Values) { $passed = $passed -and [bool]$value }
$verdict = if ($passed) { 'PASS' } else { 'FAIL' }
$acceptance = [ordered]@{
    schema = 1
    verdict = $verdict
    adaptive_quality_core_valid = [bool]$passed
    gates = $gates
    metrics = $metrics
    fatal_error = $fatal
}
Json-Write $acceptance $acceptanceJson

$summary = @"
# ARC Stage 6 Adaptive Quality Benchmark

- Source: `$sourceSha`
- Verdict: **$verdict**
- Adapter: $($manifest.adapter)
- Temporal / DLSS / FSR / Frame Generation: **OFF**
- Duration: $Seconds s
- Power scheme: $powerScheme

## Gates
- JSON valid: $($gates.json_valid)
- Temporal disabled: $($gates.temporal_disabled)
- Non-temporal actions selected: $($gates.actions_selected)
- No temporal action selected: $($gates.non_temporal_actions_only)
- Physical GPU P50 improved by >=2%: $($gates.physical_p50_improved)
- Physical GPU P99 not regressed by >0.10 ms: $($gates.physical_p99_not_regressed)

This is a deterministic ARC-owned D3D12 workload. It validates policy selection and physical GPU effect under repeatable conditions; it is not yet a claim about arbitrary-game gains.
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
}
Write-Host "Local results : $runDir"

if ($NoPublish) {
    if ($passed) { exit 0 } else { exit 2 }
}

Step 'Publishing results'
$branch = "results/stage6-$stamp"
$publishRoot = Join-Path $env:TEMP "arc-stage6-publish-$stamp"
if (Test-Path -LiteralPath $publishRoot) { Remove-Item -LiteralPath $publishRoot -Recurse -Force }
$publishError = $null
try {
    & git.exe -C $RepoRoot branch $branch $sourceSha 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git branch failed: $LASTEXITCODE" }
    & git.exe -C $RepoRoot worktree add --force $publishRoot $branch 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git worktree add failed: $LASTEXITCODE" }

    $dest = Join-Path $publishRoot ("results\stage6\" + $stamp)
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -Path (Join-Path $runDir '*') -Destination $dest -Recurse -Force -ErrorAction Stop
    & git.exe -C $publishRoot add -- "results/stage6/$stamp"
    if ($LASTEXITCODE -ne 0) { throw "git add failed: $LASTEXITCODE" }
    & git.exe -C $publishRoot commit -m "Publish Stage 6 benchmark $stamp" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git commit failed: $LASTEXITCODE" }
    & git.exe -C $publishRoot push -u origin $branch 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git push failed: $LASTEXITCODE" }

    $url = "https://github.com/Flyingzaptop/ARC/tree/$branch/results/stage6/$stamp"
    Set-Content -LiteralPath (Join-Path $localRoot 'last-result-url.txt') -Value $url -Encoding ascii
    Write-Host "`nResults URL: $url" -ForegroundColor Green
} catch {
    $publishError = $_.Exception.Message
    Write-Host "Publishing failed: $publishError" -ForegroundColor Red
} finally {
    try { & git.exe -C $RepoRoot worktree remove --force $publishRoot 2>$null | Out-Null } catch {}
}

if ($publishError) { exit 3 }
if ($passed) { exit 0 } else { exit 2 }
