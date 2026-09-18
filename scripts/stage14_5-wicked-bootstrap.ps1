param(
    [string]$RepoUrl = 'https://github.com/Flyingzaptop/ARC.git',
    [string]$Branch = 'dev/stage14_5-wicked-engine',
    [string]$WorkRoot = "$env:USERPROFILE\ARC-Stage14_5-Wicked",
    [string]$ExpectedSha = '',
    [ValidateRange(10,120)][int]$Seconds = 30,
    [switch]$NoPublish
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$WickedRepo = 'https://github.com/turanszkij/WickedEngine.git'
$WickedSha = '0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7'

function Step([string]$Text) { Write-Host ([Environment]::NewLine + "=== $Text ===") -ForegroundColor Cyan }

function Find-CMake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($installation in @(& $vswhere -products * -property installationPath)) {
            if (-not $installation) { continue }
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    return $null
}

function Ensure-CMake {
    $cmake = Find-CMake
    if ($cmake) { return $cmake }
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) { throw 'CMake is missing and winget is unavailable.' }
    Step 'Installing CMake'
    & $winget.Source install --id Kitware.CMake -e --silent --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "CMake install failed: $LASTEXITCODE" }
    $cmake = Find-CMake
    if (-not $cmake) { throw 'CMake installed but could not be located.' }
    return $cmake
}

function Find-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $msbuild = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1)
    if ($msbuild -and (Test-Path -LiteralPath $msbuild)) { return $msbuild }
    return $null
}

function Json-Write($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Pct([double]$Value) { return [math]::Round(100.0 * $Value, 1) }

if ($env:OS -ne 'Windows_NT') { throw 'Stage 14.5 requires Windows + D3D12.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'git.exe is required.' }

$arc = Join-Path $WorkRoot 'arc'
$wicked = Join-Path $WorkRoot 'WickedEngine'
New-Item -ItemType Directory -Force $WorkRoot | Out-Null

Step 'Preparing exact ARC source'
if (-not (Test-Path -LiteralPath (Join-Path $arc '.git'))) {
    & git.exe clone $RepoUrl $arc
    if ($LASTEXITCODE -ne 0) { throw "ARC clone failed: $LASTEXITCODE" }
}
& git.exe -C $arc reset --hard
& git.exe -C $arc clean -fdx
& git.exe -C $arc fetch origin $Branch --prune
if ($LASTEXITCODE -ne 0) { throw "ARC fetch failed: $LASTEXITCODE" }
& git.exe -C $arc switch -C $Branch ("origin/" + $Branch)
if ($LASTEXITCODE -ne 0) { throw "ARC switch failed: $LASTEXITCODE" }
$sourceSha = (& git.exe -C $arc rev-parse HEAD).Trim()
if ($ExpectedSha -and $sourceSha -ne $ExpectedSha) {
    throw "ARC SHA mismatch. Expected $ExpectedSha, got $sourceSha"
}
Write-Host "ARC source: $sourceSha"

$cmake = Ensure-CMake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$msbuild = Find-MSBuild
if (-not $msbuild) { throw 'MSBuild not found. Install Visual Studio / Build Tools with Desktop development with C++.' }
$env:VSLANG = '1033'
$msbuildVersionText = (& $msbuild -version -nologo | Select-Object -Last 1).Trim()
if ($msbuildVersionText -notmatch '^(\d+)\.') { throw "Unable to parse MSBuild version: $msbuildVersionText" }
$msbuildMajor = [int]$Matches[1]
if ($msbuildMajor -ge 18) { $platformToolset = 'v145' }
elseif ($msbuildMajor -ge 17) { $platformToolset = 'v143' }
else { throw "MSBuild $msbuildVersionText is too old for the Stage 14.5 C++23 bridge." }
Write-Host "MSBuild: $msbuildVersionText / $platformToolset"

Step 'Building and running ARC deterministic regressions with standard CRT'
# Test executables use the same CRT configuration as the normal Windows CI.
# Only libraries linked into Wicked need /MT; do not propagate that host ABI
# requirement into every standalone test/tool executable.
$validationBuild = Join-Path $arc 'build-validation'
& $cmake -S $arc -B $validationBuild -A x64 -DARC_BUILD_TESTS=ON -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw "ARC test configure failed: $LASTEXITCODE" }
$regressionTargets = @(
    'arc-test-assertions-tests', 'arc-wicked-telemetry-tests', 'arc-lifecycle-tests',
    'arc-resource-semantics-tests', 'arc-scene-understanding-tests',
    'arc-adaptive-quality-tests', 'arc-adaptive-quality-controller-tests',
    'arc-closed-loop-quality-sim-tests', 'arc-dx12-host-adapter-tests',
    'arc-dx12-host-adapter-surface-tests', 'arc-runtime-event-bridge-tests',
    'arc-runtime-integration-tests', 'arc-global-action-arbiter-tests',
    'arc-unified-runtime-governor-tests'
)
& $cmake --build $validationBuild --config Release --parallel 4 --target @regressionTargets
if ($LASTEXITCODE -ne 0) { throw "ARC regression build failed: $LASTEXITCODE" }
& $ctest --test-dir $validationBuild -C Release -R 'arc-(test-assertions-tests|lifecycle-tests|wicked-telemetry-tests|wicked-presentation-contract-tests|wicked-audit-boundary-tests|stage15-validation-tests|resource-semantics-tests|scene-understanding-tests|adaptive-quality-tests|adaptive-quality-controller-tests|closed-loop-quality-sim-tests|dx12-host-adapter-tests|dx12-host-adapter-surface-tests|runtime-event-bridge-tests|runtime-integration-tests|global-action-arbiter-tests|unified-runtime-governor-tests)' --output-on-failure --timeout 60
if ($LASTEXITCODE -ne 0) { throw "ARC deterministic regressions failed: $LASTEXITCODE" }

Step 'Building ARC libraries with Wicked-compatible static CRT'
$arcBuild = Join-Path $arc 'build-wicked'
if (Test-Path -LiteralPath $arcBuild) { Remove-Item -LiteralPath $arcBuild -Recurse -Force }
& $cmake -S $arc -B $arcBuild -A x64 -DARC_BUILD_TESTS=OFF -DARC_GPU_TESTS=OFF -DARC_STRESS_TESTS=OFF '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
if ($LASTEXITCODE -ne 0) { throw "ARC configure failed: $LASTEXITCODE" }
& $cmake --build $arcBuild --config Release --parallel 4 --target arc-core arc-dx12-observer
if ($LASTEXITCODE -ne 0) { throw "ARC Release build failed: $LASTEXITCODE" }

Step 'Preparing pinned Wicked Engine'
if (-not (Test-Path -LiteralPath (Join-Path $wicked '.git'))) {
    & git.exe clone --filter=blob:none --no-checkout $WickedRepo $wicked
    if ($LASTEXITCODE -ne 0) { throw "Wicked clone failed: $LASTEXITCODE" }
    & git.exe -C $wicked sparse-checkout init --cone
    if ($LASTEXITCODE -ne 0) { throw "Wicked sparse checkout init failed: $LASTEXITCODE" }
}
& git.exe -C $wicked reset --hard
& git.exe -C $wicked clean -fdx
& git.exe -C $wicked sparse-checkout set WickedEngine Samples/Tests Content
if ($LASTEXITCODE -ne 0) { throw "Wicked sparse selection failed: $LASTEXITCODE" }
& git.exe -C $wicked fetch origin $WickedSha --depth=1
if ($LASTEXITCODE -ne 0) { throw "Wicked pinned fetch failed: $LASTEXITCODE" }
& git.exe -C $wicked checkout --detach $WickedSha
if ($LASTEXITCODE -ne 0) { throw "Wicked pinned checkout failed: $LASTEXITCODE" }
$actualWicked = (& git.exe -C $wicked rev-parse HEAD).Trim()
if ($actualWicked -ne $WickedSha) { throw "Wicked SHA mismatch: $actualWicked" }

Step 'Applying ARC integration overlay'
& (Join-Path $arc 'scripts\apply-wicked-engine-integration.ps1') -WickedRoot $wicked -ArcRoot $arc -ArcBuildRoot $arcBuild
if ($LASTEXITCODE -ne 0) { throw "Wicked overlay failed: $LASTEXITCODE" }

Step 'Building real Wicked Samples/Tests renderer'
$project = Join-Path $wicked 'Samples\Tests\Tests.vcxproj'
$msbuildArgs = @(
    $project,
    '/m',
    '/p:Configuration=Release',
    '/p:Platform=x64',
    "/p:PlatformToolset=$platformToolset",
    "/p:SolutionDir=$wicked\",
    "/p:ArcRepoRoot=$arc",
    "/p:ArcBuildRoot=$arcBuild",
    '/verbosity:minimal'
)
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) { throw "Wicked Tests build failed: $LASTEXITCODE" }

$exe = Join-Path $wicked 'BUILD\x64\Release\Tests\Tests.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Wicked Tests executable missing: $exe" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$localRoot = Join-Path $arc 'results\stage14_5-wicked-local'
$runDir = Join-Path $localRoot $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
$rawJson = Join-Path $runDir 'wicked-engine.json'
$crashTxt = Join-Path $runDir 'crash-context.txt'
$manifestJson = Join-Path $runDir 'manifest.json'
$acceptanceJson = Join-Path $runDir 'acceptance.json'
$summaryMd = Join-Path $runDir 'SUMMARY.md'
$log = Join-Path $runDir 'benchmark.log'
$lastSummary = Join-Path $localRoot 'last-summary.txt'
$lastUrl = Join-Path $localRoot 'last-result-url.txt'
Remove-Item -LiteralPath $lastSummary,$lastUrl -Force -ErrorAction SilentlyContinue

Step 'Running Stage 14.5 real-renderer acceptance'
Write-Host "Wicked upstream : $WickedSha"
Write-Host "ARC source      : $sourceSha"
Write-Host 'Renderer        : Wicked Samples/Tests / D3D12'
Write-Host 'Resolution      : native 1920x1080'
Write-Host 'Temporal/upscale: OFF'
Write-Host 'Scenes          : Model, Shadows, Water, Volumetric, 65k Instances'
Write-Host "Measured time   : $Seconds s baseline + $Seconds s adaptive (+ warmup/settle/recovery)"
Write-Host 'Leave other GPU-heavy applications idle.' -ForegroundColor Yellow

$env:ARC_SOURCE_SHA = $sourceSha
$env:ARC_WICKED_SECONDS = [string]$Seconds
$env:ARC_WICKED_WARMUP_SECONDS = '5'
$env:ARC_WICKED_RECOVERY_SECONDS = '20'
$env:ARC_WICKED_SCENE_SETTLE_MS = '1500'
$env:ARC_WICKED_CONTROL_FRAMES = '16'
$env:ARC_WICKED_OUTPUT = $rawJson
$env:ARC_WICKED_CRASH_OUTPUT = $crashTxt

$started = [DateTime]::UtcNow.ToString('o')
$exit = -1
$fatal = $null
try {
    $process = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $wicked 'Samples\Tests') -PassThru -WindowStyle Hidden -ArgumentList alwaysactive
    $timeoutSeconds = ($Seconds * 2) + 180
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $fatal = "Wicked acceptance timed out after $timeoutSeconds seconds."
    } else {
        $exit = $process.ExitCode
        if ($exit -ne 0) {
            $fatal = "Wicked Tests exit code $exit"
            if (Test-Path -LiteralPath $crashTxt) {
                $crashContext = (Get-Content -LiteralPath $crashTxt -Raw).Trim()
                if ($crashContext) { $fatal += " | $($crashContext -replace '[\r\n]+','; ')" }
            }
        }
    }
} catch {
    $fatal = $_.Exception.Message
}
$finished = [DateTime]::UtcNow.ToString('o')

$raw = $null
if (Test-Path -LiteralPath $rawJson) {
    try { $raw = Get-Content -LiteralPath $rawJson -Raw | ConvertFrom-Json }
    catch { $fatal = "Invalid Wicked result JSON: $($_.Exception.Message)" }
} else {
    if (-not $fatal) { $fatal = 'Wicked renderer did not produce a result JSON.' }
    Json-Write ([ordered]@{
        schema = 1
        valid = $false
        benchmark = 'stage14_5_wicked_engine'
        integration = 'wicked_engine_tests_dx12'
        wicked_upstream_sha = $WickedSha
        arc_source_sha = $sourceSha
        fatal_error = $fatal
    }) $rawJson
}

$manifest = [ordered]@{
    schema = 1
    source_sha = $sourceSha
    wicked_repository = 'turanszkij/WickedEngine'
    wicked_sha = $WickedSha
    wicked_actual_sha = $actualWicked
    started_utc = $started
    finished_utc = $finished
    seconds_per_measurement_phase = $Seconds
    benchmark_exit_code = $exit
    fatal_error = $fatal
    crash_context_file = $(if (Test-Path -LiteralPath $crashTxt) { 'crash-context.txt' } else { $null })
}
Json-Write $manifest $manifestJson

$gates = [ordered]@{
    report_parsed = $false
    report_self_valid = $false
    exact_arc_source = $false
    exact_pinned_wicked = $false
    renderer_identity = $false
    native_1080p = $false
    temporal_disabled = $false
    gpu_timestamp_timing = $false
    semantic_labels_not_used = $false
    complex_renderer_observed = $false
    scene_coverage = $false
    resource_graph_clean = $false
    observation_control_clean = $false
    physical_quality_actuation = $false
    online_effect_learning = $false
    performance_win = $false
    bounded_churn = $false
    full_quality_recovered = $false
    backend_clean = $false
}

$metrics = [ordered]@{}
if ($raw) {
    $gates.report_parsed = $true
    $gates.report_self_valid = [bool]$raw.valid
    $gates.exact_arc_source = ([string]$raw.arc_source_sha -eq $sourceSha)
    $gates.exact_pinned_wicked = ([string]$raw.wicked_upstream_sha -eq $WickedSha -and $actualWicked -eq $WickedSha)
    $gates.renderer_identity = ([string]$raw.benchmark -eq 'stage14_5_wicked_engine' -and [string]$raw.integration -eq 'wicked_engine_tests_dx12')
    $gates.native_1080p = ([int]$raw.native_width -eq 1920 -and [int]$raw.native_height -eq 1080)
    $gates.temporal_disabled = (-not [bool]$raw.temporal_used)
    $gates.gpu_timestamp_timing = ([string]$raw.timing_source -eq 'wicked_dx12_gpu_timestamp')
    $gates.semantic_labels_not_used = (-not [bool]$raw.semantic_labels_used_by_controller)
    $gates.complex_renderer_observed = (
        [int64]$raw.host.resources -ge 128 -and
        [int64]$raw.host.descriptor_writes -ge 50 -and
        [int64]$raw.host.queue_submits -ge 500 -and
        ([int64]$raw.host.draws + [int64]$raw.host.indexed_draws) -ge 1000
    )
    $gates.scene_coverage = ([int]$raw.scene_pairs -ge 4)
    $gates.resource_graph_clean = ([int64]$raw.graph.errors -eq 0)
    $gates.observation_control_clean = ([int64]$raw.host.failed_observations -eq 0 -and [int64]$raw.host.bridge_rejections -eq 0)
    $gates.physical_quality_actuation = ([int64]$raw.governor.quality_actions_executed -ge 2 -and [int64]$raw.governor.quality_domains_executed -ge 2)
    $gates.online_effect_learning = ([int64]$raw.governor.learned_effects -ge 2)
    $gates.performance_win = [bool]$raw.performance_win
    $gates.bounded_churn = [bool]$raw.bounded_churn
    $gates.full_quality_recovered = [bool]$raw.final_quality_full
    $gates.backend_clean = ([int64]$raw.governor.quality_backend_failures -eq 0)

    $bmiss = [double]$raw.baseline.miss_ratio
    $amiss = [double]$raw.adaptive.miss_ratio
    $bp50 = [double]$raw.baseline.p50_ms
    $ap50 = [double]$raw.adaptive.p50_ms
    $bp99 = [double]$raw.baseline.p99_ms
    $ap99 = [double]$raw.adaptive.p99_ms
    $metrics = [ordered]@{
        target_frame_ms = [double]$raw.target_frame_ms
        baseline_p50_ms = $bp50
        adaptive_p50_ms = $ap50
        baseline_p99_ms = $bp99
        adaptive_p99_ms = $ap99
        baseline_miss_ratio = $bmiss
        adaptive_miss_ratio = $amiss
        miss_reduction_fraction = $(if ($bmiss -gt 0) { ($bmiss - $amiss) / $bmiss } else { 0.0 })
        p50_reduction_fraction = $(if ($bp50 -gt 0) { ($bp50 - $ap50) / $bp50 } else { 0.0 })
        scene_pairs = [int]$raw.scene_pairs
        scene_wins = [int]$raw.scene_wins
        resources = [int64]$raw.host.resources
        descriptor_writes = [int64]$raw.host.descriptor_writes
        resource_uses = [int64]$raw.host.resource_uses
        draws = [int64]$raw.host.draws
        indexed_draws = [int64]$raw.host.indexed_draws
        dispatches = [int64]$raw.host.dispatches
        queue_submits = [int64]$raw.host.queue_submits
        presents = [int64]$raw.host.presents
        quality_actions = [int64]$raw.governor.quality_actions_executed
        quality_domains = [int64]$raw.governor.quality_domains_executed
        learned_effects = [int64]$raw.governor.learned_effects
        adaptive_action_rate = [double]$raw.governor.adaptive_action_rate
        adaptive_direction_change_rate = [double]$raw.governor.adaptive_direction_change_rate
    }
}

$passed = (-not $fatal) -and ($exit -eq 0)
foreach ($value in $gates.Values) { $passed = $passed -and [bool]$value }
$verdict = $(if ($passed) { 'PASS' } else { 'FAIL' })

$acceptance = [ordered]@{
    schema = 1
    verdict = $verdict
    stage14_5_wicked_valid = [bool]$passed
    gates = $gates
    metrics = $metrics
    fatal_error = $fatal
}
Json-Write $acceptance $acceptanceJson

$summary = @"
# ARC Stage 14.5 — Wicked Engine Truth Test

- ARC source: $sourceSha
- Wicked Engine: turanszkij/WickedEngine @ $WickedSha
- Verdict: **$verdict**
- Native target: **1920x1080**
- Temporal / FSR / FSR2: **OFF**
- Timing: **Wicked's D3D12 GPU timestamp profiler**
- Controller scene labels: **NOT USED**

## Gates
- Exact pinned sources: $($gates.exact_arc_source) / $($gates.exact_pinned_wicked)
- Complex renderer observed: $($gates.complex_renderer_observed)
- Scene coverage: $($gates.scene_coverage)
- ResourceGraph clean: $($gates.resource_graph_clean)
- Observation/control clean: $($gates.observation_control_clean)
- Physical multi-domain quality: $($gates.physical_quality_actuation)
- Online effect learning: $($gates.online_effect_learning)
- Performance win: $($gates.performance_win)
- Bounded churn: $($gates.bounded_churn)
- Full quality recovered: $($gates.full_quality_recovered)
- Backend clean: $($gates.backend_clean)
"@
Set-Content -LiteralPath $summaryMd -Value $summary -Encoding utf8

$logLines = @(
    'ARC Stage 14.5 Wicked Engine truth test',
    "ARC source: $sourceSha",
    "Wicked source: $WickedSha",
    "Executable: $exe",
    "Exit code: $exit",
    "Verdict: $verdict"
)
if ($raw) {
    $logLines += @(
        "Target: $([double]$raw.target_frame_ms) ms",
        "Baseline p50: $([double]$raw.baseline.p50_ms) ms",
        "Adaptive p50: $([double]$raw.adaptive.p50_ms) ms",
        "Baseline miss: $(Pct([double]$raw.baseline.miss_ratio))%",
        "Adaptive miss: $(Pct([double]$raw.adaptive.miss_ratio))%",
        "Scene wins: $([int]$raw.scene_wins)/$([int]$raw.scene_pairs)",
        "Quality actions/domains: $([int]$raw.governor.quality_actions_executed)/$([int]$raw.governor.quality_domains_executed)",
        "Learned effects: $([int]$raw.governor.learned_effects)"
    )
}
$logLines | Set-Content -LiteralPath $log -Encoding utf8

$summaryText = "ARC Stage 14.5 Wicked $verdict"
if ($raw) {
    $summaryText += [Environment]::NewLine + "Misses: $(Pct([double]$raw.baseline.miss_ratio))% -> $(Pct([double]$raw.adaptive.miss_ratio))%"
    $summaryText += [Environment]::NewLine + "p50: $([math]::Round([double]$raw.baseline.p50_ms,3)) -> $([math]::Round([double]$raw.adaptive.p50_ms,3)) ms"
    $summaryText += [Environment]::NewLine + "Scenes won: $([int]$raw.scene_wins)/$([int]$raw.scene_pairs)"
}
$summaryText += [Environment]::NewLine + 'Results URL:'
Set-Content -LiteralPath $lastSummary -Value $summaryText -Encoding utf8

Step 'Stage 14.5 acceptance complete'
Write-Host "Verdict      : $verdict"
if ($raw) {
    Write-Host "Miss ratio   : $(Pct([double]$raw.baseline.miss_ratio))% -> $(Pct([double]$raw.adaptive.miss_ratio))%"
    Write-Host "p50          : $([math]::Round([double]$raw.baseline.p50_ms,3)) -> $([math]::Round([double]$raw.adaptive.p50_ms,3)) ms"
    Write-Host "p99          : $([math]::Round([double]$raw.baseline.p99_ms,3)) -> $([math]::Round([double]$raw.adaptive.p99_ms,3)) ms"
    Write-Host "Scenes won   : $([int]$raw.scene_wins)/$([int]$raw.scene_pairs)"
    Write-Host "Actions      : $([int]$raw.governor.quality_actions_executed) across $([int]$raw.governor.quality_domains_executed) domains"
    Write-Host "Learned      : $([int]$raw.governor.learned_effects)"
}
Write-Host "Local results: $runDir"

$publishFailed = $false
if (-not $NoPublish) {
    Step 'Publishing immutable result branch'
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $arc 'scripts\stage14_5-wicked-publish-existing.ps1') -RepoRoot $arc -RunStamp $stamp
        if ($LASTEXITCODE -ne 0) { throw "Publisher exit code $LASTEXITCODE" }
    } catch {
        $publishFailed = $true
        Write-Host "Publishing failed: $($_.Exception.Message)" -ForegroundColor Red
    }
}

if (Test-Path -LiteralPath $lastSummary) {
    Write-Host ([Environment]::NewLine + (Get-Content -LiteralPath $lastSummary -Raw))
}

if ($publishFailed) { exit 3 }
if ($passed) { exit 0 }
exit 2
