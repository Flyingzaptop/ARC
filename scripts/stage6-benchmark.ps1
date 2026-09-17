param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [ValidateRange(6,600)][int]$Seconds = 60,
    [switch]$NoPublish
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
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
$summaryMd = Join-Path $runDir 'SUMMARY.md'

$benchmarkExe = Join-Path $RepoRoot 'build\Release\dx12-adaptive-quality-benchmark.exe'
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw "Benchmark executable not found: $benchmarkExe" }

$powerScheme = ''
try { $powerScheme = (& powercfg.exe /getactivescheme 2>$null | Out-String).Trim() } catch {}
$osCaption = ''
try { $osCaption = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).Caption } catch {}

Step 'ARC Stage 6 deterministic GPU benchmark'
Write-Host "Source SHA: $sourceSha"
Write-Host "Duration: $Seconds seconds"
Write-Host 'Temporal / DLSS / FSR / Frame Generation: OFF'
Write-Host 'Please leave other GPU-heavy applications idle until the run finishes.' -ForegroundColor Yellow

$startedUtc = [DateTime]::UtcNow.ToString('o')
$exitCode = -1
try {
    & $benchmarkExe --seconds $Seconds --output $benchmarkJson 2>&1 | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
} catch {
    $_ | Out-String | Add-Content -LiteralPath $log
    throw
}
$finishedUtc = [DateTime]::UtcNow.ToString('o')
if ($exitCode -ne 0) { throw "Benchmark failed with exit code $exitCode" }
if (-not (Test-Path -LiteralPath $benchmarkJson)) { throw 'Benchmark JSON was not produced.' }

$bench = Get-Content -LiteralPath $benchmarkJson -Raw | ConvertFrom-Json
$manifest = [ordered]@{
    schema = 1
    source_sha = $sourceSha
    started_utc = $startedUtc
    finished_utc = $finishedUtc
    seconds = $Seconds
    temporal_enabled = $false
    benchmark_valid = [bool]$bench.valid
    adapter = [string]$bench.adapter
    os = $osCaption
    power_scheme = $powerScheme
}
Json-Write $manifest $manifestJson

$baseP50 = [double]$bench.baseline.p50_ms
$arcP50 = [double]$bench.adaptive.p50_ms
$baseP99 = [double]$bench.baseline.p99_ms
$arcP99 = [double]$bench.adaptive.p99_ms
$deltaP50 = $arcP50 - $baseP50
$deltaP99 = $arcP99 - $baseP99
$memoryFreedMiB = [math]::Round(([double]$bench.delta.memory_freed_bytes / 1MB), 2)
$actions = @($bench.actions)

$summary = @"
# ARC Stage 6 Adaptive Quality Benchmark

- Source: `$sourceSha`
- Adapter: $($bench.adapter)
- Temporal assistance: **OFF**
- Baseline GPU P50: $([math]::Round($baseP50,3)) ms
- Adaptive GPU P50: $([math]::Round($arcP50,3)) ms
- P50 delta: $([math]::Round($deltaP50,3)) ms
- Baseline GPU P99: $([math]::Round($baseP99,3)) ms
- Adaptive GPU P99: $([math]::Round($arcP99,3)) ms
- P99 delta: $([math]::Round($deltaP99,3)) ms
- Planned memory relief: $memoryFreedMiB MiB
- Selected actions: $($actions.Count)
- Temporal used: $($bench.temporal_used)

This is a deterministic ARC-owned D3D12 workload. It validates policy selection and physical GPU effect; it is not a claim about gains in arbitrary games.
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

Step 'Benchmark complete'
Write-Host "Baseline P50 : $([math]::Round($baseP50,3)) ms"
Write-Host "Adaptive P50 : $([math]::Round($arcP50,3)) ms"
Write-Host "P50 delta     : $([math]::Round($deltaP50,3)) ms"
Write-Host "P99 delta     : $([math]::Round($deltaP99,3)) ms"
Write-Host "Actions       : $($actions.Count)"
Write-Host "Local results : $runDir"

if ($NoPublish) { exit 0 }

Step 'Publishing results'
$branch = "results/stage6-$stamp"
$publishRoot = Join-Path $env:TEMP "arc-stage6-publish-$stamp"
if (Test-Path -LiteralPath $publishRoot) { Remove-Item -LiteralPath $publishRoot -Recurse -Force }

try {
    & git.exe -C $RepoRoot branch $branch $sourceSha
    if ($LASTEXITCODE -ne 0) { throw "git branch failed: $LASTEXITCODE" }
    & git.exe -C $RepoRoot worktree add --force $publishRoot $branch
    if ($LASTEXITCODE -ne 0) { throw "git worktree add failed: $LASTEXITCODE" }

    $dest = Join-Path $publishRoot ("results\stage6\" + $stamp)
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -LiteralPath (Join-Path $runDir '*') -Destination $dest -Recurse -Force -ErrorAction Stop
    & git.exe -C $publishRoot add -- "results/stage6/$stamp"
    if ($LASTEXITCODE -ne 0) { throw "git add failed: $LASTEXITCODE" }
    & git.exe -C $publishRoot commit -m "Publish Stage 6 benchmark $stamp"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed: $LASTEXITCODE" }
    & git.exe -C $publishRoot push -u origin $branch
    if ($LASTEXITCODE -ne 0) { throw "git push failed: $LASTEXITCODE" }

    $url = "https://github.com/Flyingzaptop/ARC/tree/$branch/results/stage6/$stamp"
    Set-Content -LiteralPath (Join-Path $localRoot 'last-result-url.txt') -Value $url -Encoding ascii
    Write-Host "`nResults URL: $url" -ForegroundColor Green
} finally {
    try { & git.exe -C $RepoRoot worktree remove --force $publishRoot 2>$null | Out-Null } catch {}
}
