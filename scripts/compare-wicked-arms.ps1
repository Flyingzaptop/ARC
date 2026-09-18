param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$WorkingDirectory,
    [Parameter(Mandatory=$true)][string]$SourceSha,
    [string]$OutputRoot = (Join-Path $PWD ('results\wicked-comparison-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [ValidateRange(3,30)][int]$Repetitions = 6,
    [ValidateRange(10,120)][int]$Seconds = 30
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stage15-validation.ps1')
if ($SourceSha -notmatch '^[0-9a-fA-F]{40}$') { throw 'SourceSha must be a full Git commit SHA.' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$binaryHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
$variables = @('ARC_SOURCE_SHA','ARC_WICKED_SECONDS','ARC_WICKED_WARMUP_SECONDS','ARC_WICKED_SCENE_SETTLE_MS','ARC_WICKED_CONTROL_FRAMES','ARC_WICKED_OUTPUT','ARC_WICKED_CRASH_OUTPUT','ARC_WICKED_EXPERIMENT_MODE','ARC_WICKED_SCENE_OFFSET','ARC_WICKED_HOOK_TIMING','ARC_WICKED_TARGET_US','ARC_WICKED_BASELINE_P50_US')
$saved = @{}
foreach ($name in $variables) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Save-Json($Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding UTF8
}
function Run-Arm([string]$Mode, [int]$Offset, [string]$Name) {
    $path = Join-Path $OutputRoot ($Name + '.json')
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite run: $path" }
    $env:ARC_WICKED_EXPERIMENT_MODE = $Mode
    $env:ARC_WICKED_SCENE_OFFSET = [string]$Offset
    $env:ARC_WICKED_OUTPUT = $path
    $env:ARC_WICKED_CRASH_OUTPUT = Join-Path $OutputRoot ($Name + '-crash.txt')
    $process = Start-Process -FilePath $Executable -WorkingDirectory $WorkingDirectory -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(($Seconds + 180) * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Comparison timed out: $Name"
    }
    if ($process.ExitCode -ne 0) { throw "Comparison failed: $Name, exit $($process.ExitCode)" }
    $result = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if (-not (Test-WickedComparisonRun $result $Mode $SourceSha $Offset)) { throw "Invalid/incomplete comparison data: $Name" }
    if ((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -ne $binaryHash) { throw 'Executable changed during comparison.' }
    return $result
}
try {
    $env:ARC_SOURCE_SHA = $SourceSha
    $env:ARC_WICKED_SECONDS = [string]$Seconds
    $env:ARC_WICKED_WARMUP_SECONDS = '5'
    $env:ARC_WICKED_SCENE_SETTLE_MS = '1500'
    $env:ARC_WICKED_CONTROL_FRAMES = '16'
    # Clock reads/atomic histograms affect hot hooks. Compare uninstrumented arms;
    # collect instrumented official diagnostics separately when needed.
    $env:ARC_WICKED_HOOK_TIMING = '0'
    $calibration = Run-Arm 'observe' 0 'calibration'
    $env:ARC_WICKED_TARGET_US = [string][int][Math]::Round(1000 * [double]$calibration.calibration_p40_ms)
    $env:ARC_WICKED_BASELINE_P50_US = [string][int][Math]::Round(1000 * [double]$calibration.calibration_p50_ms)
    if ([int]$env:ARC_WICKED_TARGET_US -le 0 -or [int]$env:ARC_WICKED_BASELINE_P50_US -le 0) { throw 'Invalid calibration target' }
    # Six permutations balance position and predecessor effects across default run.
    $orders = @(@('off','observe','adaptive'), @('observe','adaptive','off'), @('adaptive','off','observe'), @('adaptive','observe','off'), @('observe','off','adaptive'), @('off','adaptive','observe'))
    $pairs = @()
    for ($repeat = 0; $repeat -lt $Repetitions; ++$repeat) {
        $runs = @{}
        $order = $orders[$repeat % $orders.Count]
        foreach ($mode in $order) { $runs[$mode] = Run-Arm $mode ($repeat % 5) ("run-{0:D2}-{1}" -f $repeat,$mode) }
        foreach ($offScene in $runs.off.scenes) {
            $observer = @($runs.observe.scenes | Where-Object name -eq $offScene.name)[0]
            $adaptive = @($runs.adaptive.scenes | Where-Object name -eq $offScene.name)[0]
            $pairs += [pscustomobject]@{
                repeat=$repeat; scene=$offScene.name; order=($order -join ','); scene_offset=($repeat % 5)
                observer_cpu_ratio=([double]$observer.cpu_p50_ms / [double]$offScene.cpu_p50_ms)
                adaptive_cpu_ratio=([double]$adaptive.cpu_p50_ms / [double]$offScene.cpu_p50_ms)
                observer_gpu_ratio=([double]$observer.gpu_p50_ms / [double]$offScene.gpu_p50_ms)
                adaptive_gpu_ratio=([double]$adaptive.gpu_p50_ms / [double]$offScene.gpu_p50_ms)
                adaptive_vs_observer_gpu_ratio=([double]$adaptive.gpu_p50_ms / [double]$observer.gpu_p50_ms)
            }
        }
        Save-Json $pairs (Join-Path $OutputRoot 'paired-scenes.json')
    }
    $summary = [ordered]@{schema=1; acceptance_evaluated=$false; source_sha=$SourceSha; repetitions=$Repetitions; target_us=[int]$env:ARC_WICKED_TARGET_US; aggregation='arithmetic mean of per-scene per-repetition p50 ratios, equal scene weight'; off_scope='no NativeHostAdapter, exported hooks return immediately; renderer harness and hook call/branch overhead remain'; recovery_validated=$false; quality_equivalence_validated=$false; pairs=$pairs}
    foreach ($metric in @('observer_cpu_ratio','adaptive_cpu_ratio','observer_gpu_ratio','adaptive_gpu_ratio','adaptive_vs_observer_gpu_ratio')) {
        $summary[$metric] = ($pairs | Measure-Object -Property $metric -Average).Average
    }
    $summary.executable_sha256 = $binaryHash
    $summary.binary_source_binding_verified = $false
    Save-Json $summary (Join-Path $OutputRoot 'comparison.json')
    Write-Output (Join-Path $OutputRoot 'comparison.json')
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
