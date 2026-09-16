param(
    [ValidateRange(3, 30)][int]$Rounds = 6,
    [ValidateRange(8, 128)][int]$Objects = 24,
    [ValidateRange(1, 64)][int]$ObjectMiB = 2,
    [ValidateRange(1000, 1000000)][int]$ObserverIterations = 3000,
    [switch]$SkipDebugBuild,
    [switch]$Quick,
    [switch]$PublishResults
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force traces | Out-Null
    Remove-Item Env:ARC_D3D12_DEBUG -ErrorAction SilentlyContinue

    if ($Quick) {
        $Rounds = 3
        $ObserverIterations = 1500
    }

    Write-Host '=== ARC Stage 2 final acceptance ==='
    Write-Host "commit: $((git rev-parse HEAD).Trim())"
    Write-Host "rounds=$Rounds objects=$Objects objectMiB=$ObjectMiB observerIterations=$ObserverIterations"

    Write-Host ''
    Write-Host '[1/7] Release correctness + GPU + stress validation'
    & "$PSScriptRoot/validate.ps1" -Configuration Release -GpuTests -StressTests -Clean
    if ($LASTEXITCODE -ne 0) { throw "Release validation failed: $LASTEXITCODE" }

    $requiredReleaseReports = @('traces/tiled-texture-lab.json', 'traces/global-memory-lab.json')
    foreach ($path in $requiredReleaseReports) {
        if (-not (Test-Path $path)) { throw "Required Release GPU report missing: $path" }
    }
    $tiledRelease = Get-Content -Raw -LiteralPath 'traces/tiled-texture-lab.json' | ConvertFrom-Json
    $globalRelease = Get-Content -Raw -LiteralPath 'traces/global-memory-lab.json' | ConvertFrom-Json
    if (-not $tiledRelease.initial_contents_verified -or -not $tiledRelease.lower_mip_preserved -or -not $tiledRelease.promotion_contents_verified) {
        throw 'Tiled texture Release content verification failed'
    }
    if (-not $globalRelease.initial_buffer_verified -or -not $globalRelease.initial_texture_verified -or
        -not $globalRelease.lower_mip_preserved -or -not $globalRelease.restored_buffer_verified -or
        -not $globalRelease.restored_texture_verified) {
        throw 'Global memory Release content verification failed'
    }
    if ($globalRelease.eviction_actions -lt 1 -or $globalRelease.demotion_actions -lt 1 -or
        $globalRelease.make_resident_actions -lt 1 -or $globalRelease.promotion_actions -lt 1) {
        throw 'Global memory lab did not exercise all four action classes'
    }

    if (-not $SkipDebugBuild) {
        Write-Host ''
        Write-Host '[2/7] Debug correctness + GPU validation'
        & "$PSScriptRoot/validate.ps1" -Configuration Debug -GpuTests -Clean
        if ($LASTEXITCODE -ne 0) { throw "Debug validation failed: $LASTEXITCODE" }
    } else {
        Write-Host '[2/7] Debug build skipped by request'
    }

    Write-Host ''
    Write-Host '[3/7] D3D12 debug-layer probe and validation'
    $debugLayerAvailable = $false
    $debugLayerClean = $false
    $env:ARC_D3D12_DEBUG = '1'
    & ./build/Release/dx12-global-memory-lab.exe
    $debugProbeCode = $LASTEXITCODE
    Remove-Item Env:ARC_D3D12_DEBUG -ErrorAction SilentlyContinue
    if ($debugProbeCode -eq 0) {
        $debugLayerAvailable = $true
        $debugReport = Get-Content -Raw -LiteralPath 'traces/global-memory-lab.json' | ConvertFrom-Json
        $debugLayerClean = ([int64]$debugReport.debug_error_count -eq 0)
        if (-not $debugLayerClean) { throw "Global memory debug-layer errors: $($debugReport.debug_error_count)" }
        & "$PSScriptRoot/validate.ps1" -Configuration Release -GpuTests -DebugLayer
        if ($LASTEXITCODE -ne 0) { throw "Full debug-layer validation failed: $LASTEXITCODE" }
    } elseif ($debugProbeCode -eq 77) {
        Write-Host 'D3D12 debug layer unavailable: external validation remains pending.' -ForegroundColor Yellow
    } else {
        throw "D3D12 debug-layer probe failed: $debugProbeCode"
    }

    Write-Host ''
    Write-Host '[4/7] Observer overhead regression benchmark'
    & "$PSScriptRoot/benchmark.ps1" -Iterations $ObserverIterations -Rounds 3
    if ($LASTEXITCODE -ne 0) { throw "Observer benchmark failed: $LASTEXITCODE" }

    Write-Host ''
    Write-Host '[5/7] Autonomous residency baseline/oracle/auto benchmark'
    & "$PSScriptRoot/residency-benchmark.ps1" -Rounds $Rounds -Objects $Objects -ObjectMiB $ObjectMiB
    if ($LASTEXITCODE -ne 0) { throw "Residency benchmark failed: $LASTEXITCODE" }

    Write-Host ''
    Write-Host '[6/7] Multi-pattern memory/latency frontier'
    $frontierRounds = if ($Quick) { 1 } else { [Math]::Max(2, [Math]::Min(3, [int][Math]::Ceiling($Rounds / 3.0))) }
    & "$PSScriptRoot/residency-frontier.ps1" -Rounds $frontierRounds -Objects $Objects -ObjectMiB $ObjectMiB -WarmupEpochs 300 -MeasuredEpochs 1200
    if ($LASTEXITCODE -ne 0) { throw "Residency frontier failed: $LASTEXITCODE" }

    Write-Host ''
    Write-Host '[7/7] Hardware calibration and acceptance report'
    & "$PSScriptRoot/calibrate.ps1" -Output 'traces/hardware-profile.json' -StorageFile 'build/Release/arc-calibrate.exe'
    if ($LASTEXITCODE -ne 0) { throw "Calibration failed: $LASTEXITCODE" }

    $expected = @(
        'traces/benchmark-matrix.json',
        'traces/benchmark-summary.json',
        'traces/residency-benchmark.json',
        'traces/residency-benchmark-summary.json',
        'traces/residency-frontier.json',
        'traces/residency-frontier-summary.json',
        'traces/residency-frontier.csv',
        'traces/tiled-texture-lab.json',
        'traces/global-memory-lab.json',
        'traces/hardware-profile.json'
    )
    foreach ($path in $expected) {
        if (-not (Test-Path $path)) { throw "Expected artifact missing: $path" }
    }

    $observer = Get-Content -Raw -LiteralPath 'traces/benchmark-summary.json' | ConvertFrom-Json
    $residency = Get-Content -Raw -LiteralPath 'traces/residency-benchmark-summary.json' | ConvertFrom-Json
    $frontier = Get-Content -Raw -LiteralPath 'traces/residency-frontier-summary.json' | ConvertFrom-Json
    $tiled = Get-Content -Raw -LiteralPath 'traces/tiled-texture-lab.json' | ConvertFrom-Json
    $global = Get-Content -Raw -LiteralPath 'traces/global-memory-lab.json' | ConvertFrom-Json
    $hardware = Get-Content -Raw -LiteralPath 'traces/hardware-profile.json' | ConvertFrom-Json

    $observerP99TargetMs = 0.15
    $observerCpuTargetPercent = 2.0
    $observerP99Met = ([double]$observer.light_delta.p99_ms -le $observerP99TargetMs)
    $observerCpuMet = ($null -ne $observer.light_delta.cpu_percent -and [double]$observer.light_delta.cpu_percent -le $observerCpuTargetPercent)
    $frontierPointFound = ($null -ne $frontier.recommended_lab_point)
    $globalDxgiEvidence = ([int64]$global.dxgi_observed_relief_bytes -gt 0)
    $globalActionMix = ([int]$global.eviction_actions -gt 0 -and [int]$global.demotion_actions -gt 0 -and
                        [int]$global.make_resident_actions -gt 0 -and [int]$global.promotion_actions -gt 0)
    $globalDataOk = ([bool]$global.initial_buffer_verified -and [bool]$global.initial_texture_verified -and
                     [bool]$global.lower_mip_preserved -and [bool]$global.restored_buffer_verified -and
                     [bool]$global.restored_texture_verified)
    $tiledDataOk = ([bool]$tiled.initial_contents_verified -and [bool]$tiled.lower_mip_preserved -and [bool]$tiled.promotion_contents_verified)
    $hardCorrectness = ($globalActionMix -and $globalDataOk -and $tiledDataOk)

    $verdict = 'ACCEPTED'
    if (-not $hardCorrectness) {
        $verdict = 'NOT_ACCEPTED'
    } elseif (-not $debugLayerAvailable) {
        $verdict = 'ACCEPTED_WITH_EXTERNAL_VALIDATION_PENDING'
    } elseif (-not $frontierPointFound -or -not $observerP99Met -or -not $observerCpuMet) {
        $verdict = 'ACCEPTED_WITH_PERFORMANCE_WARNINGS'
    }

    $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{ name=$_.Name; driver_version=$_.DriverVersion; adapter_ram=$_.AdapterRAM }
    })
    $os = Get-CimInstance Win32_OperatingSystem
    $commit = (git rev-parse HEAD).Trim()
    $trackedStatus = (git status --porcelain --untracked-files=no | Out-String).Trim()
    $dirty = -not [string]::IsNullOrWhiteSpace($trackedStatus)

    $acceptance = [ordered]@{
        schema = 1
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        commit = $commit
        working_tree_dirty = $dirty
        verdict = $verdict
        machine = [ordered]@{
            computer_name = $env:COMPUTERNAME
            os_caption = $os.Caption
            os_build = $os.BuildNumber
            gpu = $gpu
        }
        gates = [ordered]@{
            hard_correctness = $hardCorrectness
            tiled_content_integrity = $tiledDataOk
            global_content_integrity = $globalDataOk
            global_action_mix = $globalActionMix
            global_dxgi_relief_evidence = $globalDxgiEvidence
            debug_layer_available = $debugLayerAvailable
            debug_layer_clean = $debugLayerClean
            frontier_operating_point_found = $frontierPointFound
            observer_light_p99_target_ms = $observerP99TargetMs
            observer_light_p99_delta_ms = [double]$observer.light_delta.p99_ms
            observer_light_p99_target_met = $observerP99Met
            observer_light_cpu_target_percent = $observerCpuTargetPercent
            observer_light_cpu_delta_percent = $observer.light_delta.cpu_percent
            observer_light_cpu_target_met = $observerCpuMet
        }
        global_memory = $global
        tiled_texture = $tiled
        observer = $observer
        residency = $residency
        frontier = $frontier
        hardware_profile_path = 'traces/hardware-profile.json'
    }
    $acceptance | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath 'traces/stage2-final-acceptance.json' -Encoding utf8

    $recommendedText = 'none'
    if ($frontierPointFound) {
        $recommendedText = "managed=$($frontier.recommended_lab_point.managed_percent)% resident_fraction=$([Math]::Round([double]$frontier.recommended_lab_point.mean_full_working_set_fraction, 3)) worst_p99_delta_ms=$([Math]::Round([double]$frontier.recommended_lab_point.worst_p99_delta_ms, 4))"
    }
    $cpuDeltaText = if ($null -eq $observer.light_delta.cpu_percent) { 'n/a' } else { [Math]::Round([double]$observer.light_delta.cpu_percent, 3) }
    $report = @"
# ARC Stage 2 Final Acceptance

- Verdict: **$verdict**
- Commit: `$commit`
- Timestamp UTC: $($acceptance.timestamp_utc)
- Working tree dirty: $dirty

## Correctness gates

- Tiled mip content integrity: $tiledDataOk
- Global memory content integrity: $globalDataOk
- Global action mix (Evict + Demote + MakeResident + Promote): $globalActionMix
- DXGI physical-relief evidence: $globalDxgiEvidence (`$($global.dxgi_observed_relief_bytes)` bytes observed)
- D3D12 debug layer available: $debugLayerAvailable
- D3D12 debug layer clean: $debugLayerClean

## Performance gates

- Light observer P99 delta: $([Math]::Round([double]$observer.light_delta.p99_ms, 4)) ms (target <= $observerP99TargetMs ms): $observerP99Met
- Light observer CPU delta: $cpuDeltaText % (target <= $observerCpuTargetPercent %): $observerCpuMet
- Safe frontier point found: $frontierPointFound
- Frontier recommendation: $recommendedText

## Global memory lab

- Requested relief: $($global.requested_relief_bytes) bytes
- Planned relief: $($global.planned_relief_bytes) bytes
- DXGI observed relief: $($global.dxgi_observed_relief_bytes) bytes
- Eviction actions: $($global.eviction_actions)
- Texture demotions: $($global.demotion_actions)
- Make-resident actions: $($global.make_resident_actions)
- Texture promotions: $($global.promotion_actions)

## Artifacts

- `traces/stage2-final-acceptance.json`
- `traces/benchmark-summary.json`
- `traces/residency-benchmark-summary.json`
- `traces/residency-frontier-summary.json`
- `traces/residency-frontier.csv`
- `traces/tiled-texture-lab.json`
- `traces/global-memory-lab.json`
- `traces/hardware-profile.json`
"@
    $report | Set-Content -LiteralPath 'traces/STAGE2_FINAL_ACCEPTANCE.md' -Encoding utf8

    Write-Host ''
    Write-Host '=== FINAL VERDICT ==='
    Write-Host $verdict
    Write-Host "Acceptance JSON: traces/stage2-final-acceptance.json"
    Write-Host "Acceptance report: traces/STAGE2_FINAL_ACCEPTANCE.md"

    if ($verdict -eq 'NOT_ACCEPTED') { exit 2 }

    if ($PublishResults) {
        Write-Host ''
        Write-Host 'Publishing curated acceptance artifacts to an isolated results branch...'
        & "$PSScriptRoot/publish-stage2-results.ps1"
        if ($LASTEXITCODE -ne 0) { throw "Results publication failed: $LASTEXITCODE" }
        if (Test-Path 'traces/stage2-results-url.txt') {
            Write-Host "Results URL: $((Get-Content -Raw -LiteralPath 'traces/stage2-results-url.txt').Trim())"
        }
    }
} finally {
    Remove-Item Env:ARC_D3D12_DEBUG -ErrorAction SilentlyContinue
    Pop-Location
}
